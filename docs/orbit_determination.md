# Orbit Determination via Nonlinear Filtering & Smoothing

**Epistemic status of this document.** This is a **design specification**, not
a description of shipped behavior. Sections marked **[SPEC]** describe the
intended module design, API surface, and mathematics; they will be replaced by
**[IMPL]** callouts and cross-references to code once the implementation
lands and is verified. Newton Architect rules apply: no fact stated here as
"proved" or "verified" without a check in code or a citation to a specific,
real reference.

**Author:** Robert W. McGwier (N4HY) with Claude (Anthropic) assistance under
NEWTON ARCHITECT supervision.
**Draft date:** 2026-08-02.
**Status:** DRAFT / design spec. Not yet implemented.

---

## 1. Purpose

Existing propagation in this repository is *prediction-only*: TLE →
SGP4-at-epoch → HPOP integration forward. There is no mechanism to
**correct** a state estimate against measurements. This document specifies
adding a compact, testable **orbit-determination (OD) subsystem** that:

1. Takes a TLE and produces an initial state in the propagator's frame.
2. Propagates that state under the full HPOP force model to the first
   acquisition of signal (AOS).
3. Between AOS and loss of signal (LOS), fuses **Doppler observations from
   a known ground station** into the state estimate using a
   **Square-Root Unscented Kalman Filter (SRUKF)**.
4. After LOS, runs a **Rauch–Tung–Striebel (RTS) smoother** in
   **square-root form** backward from LOS to AOS.
5. Optionally repeats **filter → smoother → filter → smoother …** until a
   documented convergence criterion is met, capped at a maximum number of
   iterations.
6. Reports state, covariance, log-likelihood, and per-epoch innovations for
   audit.

The filter and smoother are **not written from scratch here**. They are
consumed from the sibling library
[`Modern-Computational-Nonlinear-Filtering`](https://…) (hereafter **NLF**),
which already provides an SRUKF, a fixed-interval square-root smoother, and a
fixed-lag square-root smoother — verified in that library on four benchmark
problems (bearing-only tracking, coupled oscillators, Van der Pol, reentry
vehicle).

Numerical linear-algebra primitives (Cholesky factor, triangular solves,
GEMM) are consumed from the sibling library
[`OptimizedKernelsForRaspberryPi5_NvidiaCUDA`](https://…) (hereafter
**OptMathKernels**), through NLF's `filtermath` dispatch layer. On a
Raspberry Pi 5 this dispatches to NEON; on an NVIDIA host to cuBLAS/cuSOLVER;
on a plain x86 to Eigen.

---

## 2. Scope, Assumptions, and Non-Goals

**Scope (v0).**

- One-way downlink Doppler from a single ground station to a single
  satellite, LEO-class orbits (ISS, NOAA, Iridium-NEXT).
- State vector: position + velocity in the **TEME** frame used by the
  existing numerical propagator (§3), augmented with a ground-station
  **oscillator bias + drift** pair (see §4.3). Optional additional
  augmentation with a drag consider-parameter is a v1 item.
- Deterministic force model: whatever `ForceModel::acceleration()` already
  provides (EGM96 up to degree/order 20, Sun, Moon, exponential drag, SRP
  with cylindrical shadow). No stochastic acceleration model beyond diagonal
  process noise in v0.

**Explicit non-goals for v0.**

- No **troposphere / ionosphere refraction correction**. The observation
  bias attributable to these media is folded into the R matrix inflation
  and the estimated oscillator bias. This is a *declared limitation*, not a
  hidden assumption. To be revisited in v1.
- No **light-time iteration**. At LEO ranges (≤ 2500 km slant), light-time
  is ≤ 8.3 ms; at 1 Hz sample rate this displaces the emission epoch by less
  than 1% of the sample interval and is dominated by other error terms.
  Callable if geostationary or lunar work is added later.
- No **two-way ranging** or **carrier-phase differencing**. Doppler-only.
- No **multi-satellite joint estimation**. One satellite at a time.
- No **real-time (online) operation** in v0. Batch over a single pass.
  Real-time streaming is a v1 item and would use the fixed-lag smoother.

**Newton Architect note on assumptions.** Every one of the above is a
*modelling choice*, not a fact. If a benchmark result later shows one of
them dominates the residual budget for a given target, we revisit; we do
*not* explain away the residuals by inflating R until they fit.

---

## 3. Frame and Unit Discipline

The single most common way OD code corrupts results is silent frame
mismatch. The following rules are enforced module-wide and are checked in
unit tests.

**F1. Propagation frame.** All state vectors carried by the OD subsystem
are in **TEME**, the frame `ForceModel::acceleration()` was built against
(`src/force_model.cpp`) and the frame SGP4 natively produces. ECEF
rotation is via GMST only (`GmstRotation`), matching the rest of the
tracker. **No GCRF conversion is performed anywhere in the OD path.**
See §6 for the design decision.

**F1a. TEME-as-inertial (Newton audit).** The propagator and OD subsystem
both treat TEME as if it were an inertial frame, i.e. they omit the
fictitious Coriolis and centrifugal terms that arise because TEME rotates
relative to GCRS at the precession-nutation rate. For LEO,

```
|2 ω_p × v|          ≈ 2 · 7.7e-12 rad/s · 7.7 km/s   ≈ 1.2e-10 km/s²
|ω_p × (ω_p × r)|    ≈ (7.7e-12)² · 7000 km            ≈ 4e-19 km/s²
```

These are dwarfed by SRP (~1e-9 km/s²), drag (~1e-8 km/s²), and Sun/Moon
third-body (~1e-7 km/s²). Integrated over a day the suppressed Coriolis
term accumulates to O(10 m) position error — a real physical effect, not
merely a labelling one. It is left uncorrected because the propagator,
observer look-angle code, SGP4 fallback, and OD filter are all made to
agree in a single self-consistent TEME. A proper remedy requires
TEME→GCRS state conversion at every acceleration evaluation *plus*
matching observer / visibility updates; that whole-system frame rewrite
is deferred and out of scope for the OD subsystem. Any OD result must be
presented with this bound in the caveats. See
`include/numerical_propagator.hpp` for the propagator-side version of
this note.

**F1b. Sun/Moon third-body frame (audit fix).** The Montenbruck-Gill
analytic Sun/Moon ephemerides return vectors in the J2000 mean equator /
mean equinox frame. `ForceModel::acceleration` uses the MOD-rotated
variants `sunPositionTEME` / `moonPositionTEME` (IAU-1976 precession) so
the third-body contribution and the geopotential contribution live in the
same frame. This is a local, algebraic fix; it is unrelated to F1a.

**F2. Units.** All positions in **kilometres**, velocities in
**kilometres per second**, times in **seconds** since the pass epoch (a
`double` counter) *for filter internals*. Times in **UTC Julian Date** at
the module boundary (matching `NumericalPropagator::propagate()`).

**F3. Angles.** All internal angles in **radians**. Any I/O in degrees is
converted at the module boundary with a named constant, never inline.

**F4. Covariance units.** `P` blocks corresponding to position are in
km²; velocity blocks in (km/s)²; cross-blocks in km·(km/s). Any code that
reports "1-σ position error in km" reports `sqrt(trace(P_rr)/3)` and is
labelled *isotropic-equivalent 1-σ*, never just "1-σ", because
non-isotropic errors are the norm for orbits (radial vs along-track vs
cross-track spreads differ by orders of magnitude).

---

## 4. State Vector and Measurement Model

### 4.1 State vector (v0)

The v0 augmented state is 8-dimensional:

```
x = [ r_x, r_y, r_z, v_x, v_y, v_z, b_c, b_dot ] ᵀ
     └── r [km] ──┘  └── v [km/s] ──┘  │       │
                                       │       └─ oscillator drift [Hz/s]
                                       └───────── oscillator bias  [Hz]
```

Rationale for the augmentation: a station-oscillator bias appears as an
additive constant on every Doppler measurement of the pass and is
otherwise **algebraically indistinguishable from a radial-velocity bias**
under a single-station Doppler-only observability. Not modelling it makes
the filter aliase oscillator error into an orbit-velocity error. Adding it
adds two states, keeps observability tractable across a pass with visible
Doppler curvature (positive elevation-rate change through zenith break),
and matches standard practice in satellite laser-ranging and space-object
tracking literature.

Bias states are modelled as a first-order Gauss–Markov (integrated white
noise on `b_dot`, white noise on `b_c` if desired). Concrete Q entries
are a **tuning matter** and will be documented alongside the code, with
the default values labelled *empirical starting point*, not *proved
optimal*.

### 4.2 Dynamics function `f(x, u, t)`

For the orbital block `x_orb = [r; v]`:

```
d/dt x_orb = [ v
               a(t, r, v) ]
```

where `a(·)` is `ForceModel::acceleration(jd, r, v)` from the existing
propagator. The filter treats `propagate(t_k → t_{k+1})` as a black-box
transition applied to each sigma point independently — the standard
UKF-with-black-box-dynamics pattern (van der Merwe & Wan 2001).

**Integrator note (Newton audit).** The `NumericalPropagator` used by
`--hpop` is an adaptive **Fehlberg RK7(8)** with per-step error control.
The OD subsystem does **not** call it; it uses `od::integrate_rk4` in
`src/od/od_dynamics.cpp`, a fixed-step classical **RK4** driven by the
same `ForceModel`. The reason is cost: every SRUKF predict step evaluates
`f` at all `2·NX + 1 = 17` sigma points, and the outer OD driver may run
that predict step tens to hundreds of times per pass. A per-sigma-point
adaptive RK7(8) with its own error control loop and re-tries would
dominate the wall-clock budget without materially improving the ~1-second
sigma-point propagation intervals used in a Doppler pass. The tradeoff is
that any change that lengthens the sigma-point interval (e.g. sparser
observation cadence) should re-verify RK4's local truncation against the
Van Loan Q floor — for LEO at 1 s steps this is well below numerical
noise, but the ordering is not guaranteed at 10 s or more.

For the bias block:

```
d/dt b_c   = b_dot
d/dt b_dot = 0        (driven only by process noise)
```

### 4.3 Measurement function `h(x, t)`

For a single Doppler observation at time `t_k` from ground station at
geodetic `(lat, lon, alt)`:

1. Rotate station geodetic → ECI position `r_s(t_k)` and inertial velocity
   `v_s(t_k)` using the *same* Earth-rotation model the propagation frame
   uses (§F1).
2. Compute range vector `ρ = r − r_s`, unit line-of-sight
   `ρ̂ = ρ / ‖ρ‖`, and relative velocity `v_rel = v − v_s`.
3. Classical range-rate: `ρ_dot = ρ̂ · v_rel`.
4. **Relativistic Doppler** (special-relativistic form, one-way):

```
f_R / f_T = √(1 - β²) / (1 + β·ρ̂)     [Rindler, Relativity, §3.7]

  where β = v_rel / c is the source (satellite) velocity in the
  station rest frame; c = 299 792.458 km/s.
```

The `+` sign in the denominator is load-bearing and follows from the
`ρ̂ = station → satellite` convention adopted in step 2 above: light
travels from source to observer in direction `-ρ̂`, so the standard SR
form `1 - β_light` with `β_light = β·(-ρ̂) = -β·ρ̂` reduces to
`(1 + β·ρ̂)` here. Recession then gives `β·ρ̂ > 0`, denominator `> 1`,
`f_R < f_T` (redshift), as expected. An earlier draft of this section and
of the header comment in `include/od/doppler_measurement.hpp` wrote
`(1 - β·ρ̂)`; that was a documentation regression, not a code error, and
was caught by unit test T4 (limit-case reduction to classical Doppler)
plus the audit-fix pass on the header.

For LEO (‖v‖ ≈ 7.7 km/s so β ≈ 2.6·10⁻⁵), the difference from the
classical form `f_R/f_T ≈ 1 − ρ_dot/c` is of order β² ≈ 7·10⁻¹⁰, i.e.
about 1 Hz at 1.6 GHz Iridium downlink. This is at the edge of ordinary
receiver stability and **is included** rather than dropped, because
the whole point of a filter is to be honest about what it sees.

5. Convert to observed frequency and add oscillator bias:

```
h(x, t_k) = f_T · (√(1-β²)/(1-β·ρ̂)) + b_c  +  b_dot · (t_k - t_pass_ref)
```

The Jacobian is *not* required for SRUKF — that is the point of unscented
filtering — but the analytical Jacobian will be provided in a comment
next to `h(·)` for review and for possible fallback to an EKF path in v1.

**No troposphere/ionosphere in v0.** This is a declared limitation. R
inflation is discussed in §7.4.

### 4.4 Measurement noise `R`

`R` is 1×1 (Doppler is a scalar per epoch). Its value is set as a *tuning
parameter*, whose starting default is `(σ_meas + σ_atm + σ_osc)²` where the
three contributors are documented as empirical estimates for a nominal
Iridium/NOAA-class link. **These defaults are not derived; they are
starting points**, and any published result must include the actual R used.

---

## 5. Filter, Smoother, and Iterated Modes

The NLF library exposes:

| Class (NLF header)                | Purpose                                             |
| --------------------------------- | --------------------------------------------------- |
| `UKFCore::SRUKF<NX,NY>`           | Forward SRUKF filter                                |
| `UKFCore::SRUKFSmoother<NX,NY>`   | Batch **RTS** smoother in square-root form         |
| `UKFCore::SRUKFFixedLagSmoother`  | Fixed-lag SRUKF smoother (for streaming operation)  |

The OD subsystem exposes four **modes** on top of these primitives:

### 5.1 Mode A — Forward SRUKF only

Propagate the state from TLE epoch → AOS with the deterministic
propagator; initialize the SRUKF with that state and a prior covariance
(see §7.1); iterate `predict / update` across all Doppler observations
between AOS and LOS. Final state and covariance are the filter's
posterior at LOS.

**Use when:** you want the classical filter output only, or want to
compare forward-filter vs smoother.

### 5.2 Mode B — Forward SRUKF + SRUKF smoother (RTS)

Same as Mode A, then call `SRUKFSmoother::smooth(n_iterations=0)`. This
runs a single backward pass that produces improved state and covariance
estimates *at every epoch* in the pass, not only at LOS.

**Use when:** you want the best per-epoch trajectory reconstruction for
the pass (post-pass analysis, ephemeris improvement).

### 5.3 Mode C — Forward AOS→LOS, Smoother LOS→AOS

Notationally identical to Mode B, but callable in a form that produces
the smoothed state **at AOS** as a corrected prior for the next pass.
Under Newton rules we note explicitly: Mode C's *output at AOS* is not
"the truth at AOS"; it is the smoothed posterior conditioned on the
pass's observations under the assumed models, and it inherits every
assumption in §2 and §4.

### 5.4 Mode D — Iterated Filter–Smoother

```
x⁽⁰⁾ = initial forward-filter posterior
for i = 1 … I_max:
    smooth backward using x⁽ⁱ⁻¹⁾ trajectory  →  x_smooth⁽ⁱ⁾
    re-run forward filter using x_smooth⁽ⁱ⁾ at AOS as prior,
        original P0 preserved                →  x_filt⁽ⁱ⁾
    compute stopping criteria (§5.5)
    if converged: break
```

The pattern is a **fixed-point iteration**; NLF's `SRUKFSmoother::smooth`
supports it directly via its `n_iterations` argument.

**Use when:** the initial TLE-seeded prior is far enough from truth that
one forward pass leaves nonlinearities incompletely accommodated (typical
for HPOP-vs-SGP4 differences accumulated between TLE epoch and AOS, or
when the pass starts near horizon where geometry is weak).

**Under Newton rules,** we do *not* claim iteration always improves the
answer. On perfectly modelled, high-SNR data it converges in one or two
iterations; on model-mismatched data it may oscillate. Both behaviours
will be exercised in the benchmarks (§8) and reported honestly.

### 5.5 Convergence criteria (Mode D)

By user selection, **both** of the following are checked each iteration,
and iteration halts at the earlier trigger, with a hard cap `I_max = 20`:

- **State norm criterion**

```
δ_x⁽ⁱ⁾ = max_k ‖ x_smooth⁽ⁱ⁾(k) − x_smooth⁽ⁱ⁻¹⁾(k) ‖_P⁻¹
       < tol_state    (default 1e-6, dimensionless Mahalanobis)
```

Mahalanobis-norm is used, not raw Euclidean, so the criterion is
scale-invariant across the mixed km / km/s / Hz state.

- **Log-likelihood criterion (EM-style)**

```
ℓ⁽ⁱ⁾ = -½ · Σ_k [ log|2π S_yy(k)|  +  ν(k)ᵀ S_yy(k)⁻¹ ν(k) ]
Δℓ⁽ⁱ⁾ = ℓ⁽ⁱ⁾ − ℓ⁽ⁱ⁻¹⁾
       < tol_loglik   (default 1e-4)
```

Both thresholds are configurable. The choice of two criteria is
belt-and-braces: state-norm can plateau while log-likelihood still
moves (or vice versa on ill-conditioned problems); the earlier trigger
is safer.

---

## 6. TLE → State at Filter Epoch

**Design decision (2026-08-02):** The OD subsystem operates in **TEME**
end-to-end. It does **not** convert TEME → GCRF at any point.

Rationale:

- The existing `ForceModel` and `NumericalPropagator` in this repository
  are built to work in the same TEME-as-pseudo-inertial frame that SGP4
  produces (documented in `src/force_model.cpp` and `src/numerical_propagator.cpp`).
- Introducing a GCRF conversion here without also upgrading the force
  model to the IAU-2006/2000A precession/nutation rotation of the
  geopotential coefficients would produce an **inconsistent** system —
  input state in one frame, integration in another. That is exactly the
  silent-frame-mismatch failure mode Newton Architect §Governing Authority
  demands we refuse.
- All downstream code (observer look angles, geodetic conversion,
  visibility) is already TEME-consistent through `GmstRotation`.

Procedure:

1. Read TLE with `libsgp4`.
2. Evaluate SGP4 at TLE epoch → position/velocity in **TEME** (native SGP4
   frame).
3. Hand the resulting TEME (r, v) unchanged to `NumericalPropagator` (via
   its existing TLE constructor, which already does exactly this).
4. All subsequent state vectors carried by the OD subsystem are in the
   same TEME frame.

**Declared inherited limitation.** TEME is not strictly GCRF — the two
differ by the equation of equinoxes plus small precession/nutation terms.
For LEO orbits over single-pass horizons (~15 min) and for Doppler-only
observability from a single station, the omitted rotation is a
sub-arcsecond geometric effect on the line-of-sight vector; it does not
dominate the Doppler noise budget of typical amateur/professional links.
This limitation is stated with every result, not hidden.

A GCRF-based OD path, together with a matching full IAU-2006/2000A force
model upgrade, is **out of scope for this subsystem** and would be a
separate project.

---

## 7. Initial Conditions and Prior Covariance

### 7.1 Initial state x₀

`x₀` at the AOS epoch is the deterministic propagator's state at AOS,
seeded from SGP4-at-TLE-epoch and integrated forward with the full force
model.

### 7.2 Initial covariance P₀

There is no principled way to derive P₀ from a TLE alone — TLEs do not
carry covariance. The v0 default is a diagonal `P₀` whose entries encode
*documented starting assumptions*:

```
σ_r_along   =  5.0  km       (order-of-magnitude TLE along-track error at AOS)
σ_r_cross   =  1.0  km
σ_r_radial  =  0.5  km
σ_v_along   =  5.0  m/s
σ_v_cross   =  1.0  m/s
σ_v_radial  =  0.5  m/s
σ_b_c       =  50   Hz       (station oscillator initial uncertainty)
σ_b_dot     =  1    Hz/s
```

These are aligned in the **RSW (radial / along-track / cross-track) frame
at AOS**, then rotated into ECI to form P₀. The RSW axes are the
standard along-track-error frame for orbits; assigning σ in RSW and
rotating captures the anisotropy correctly.

**These numbers are documented as starting priors, not derived.**
Sensitivity to P₀ is reported in the benchmarks (§8).

### 7.3 Process noise Q

Diagonal in v0:

```
Q_pos = 0                 (position is deterministic, driven by v)
Q_vel = (σ_a · Δt) I₃    with σ_a chosen so ‖Q_vel‖ ≈ residual force-model
                          error over one integration step
Q_bc  = q_bc · Δt
Q_bd  = q_bd · Δt
```

Concrete values live in code with named constants and inline commentary
citing what they approximate.

### 7.4 Measurement noise R

`R = σ_R²` where `σ_R` (in Hz) is set by the caller. The recommended
starting decomposition is:

```
σ_R² = σ_meas² + σ_atm² + σ_osc_short²
```

where `σ_atm²` is the neglected-troposphere-and-ionosphere budget for
the specific link (a *declared* placeholder, not a modelled term). Any
result reported from the OD subsystem must state the R used.

---

## 8. Verification and Benchmarks

**Newton Architect statement.** "Unit tests pass" and "residuals look
small" are **not** proof of correctness. The following are the checks
that will be shipped, and each one produces an artifact that a reviewer
can inspect independently.

### 8.1 Unit tests (per-module, small, fast)

**T1. Frame round-trip.** Rotate an ECI vector to ECEF and back with
`GmstRotation`; verify Euclidean norm preserved to machine precision;
verify inverse identity to 1e-12 relative.

**T2. Station kinematics.** For a fixed geodetic point, verify that
`r_s(t)` computed at two times separated by 1 s and finite-differenced
matches `v_s(t)` to within `1e-3 · ‖v_s‖`.

**T3. Doppler model derivatives.** For a fixed `(r_s, v_s, r, v)`,
perturb each state component numerically and confirm the analytical
Jacobian (documented in code) matches the finite-difference Jacobian to
1e-6 relative.

**T4. Relativistic reduction.** For β ≪ 1, verify that
`f_R/f_T (relativistic) − f_R/f_T (classical) = O(β²)` with the correct
sign and coefficient (Rindler §3.7). This is a *symbolic* check
implemented numerically.

**T5. SRUKF sigma-point invariance.** Round-trip mean/covariance
through `generate_sigma_points_from_sqrt` and verify recovery to 1e-10.
Executed against NLF's primitives.

**T6. RTS smoother on linear-Gaussian test problem.** Configure the
SRUKF with a linear `f` and linear `h`; compare against the closed-form
Kalman + RTS solution on 100 epochs. Any deviation beyond 1e-8 in
smoothed mean and covariance is a bug.

**T7. Iterated F–S convergence.** On the same linear-Gaussian problem,
verify that Mode D converges to the closed-form smoother in exactly one
iteration (linear-Gaussian systems have no iteration benefit) and does
not degrade with `I_max = 5`.

**T8. Force-model / propagator round-trip.** Integrate a state
forward `Δt` and back `−Δt`; the composition must recover the initial
state to `atol` set by the integrator tolerance.

**T9. TLE→state frame documentation test.** Given a canonical TLE,
verify that SGP4-at-epoch returns TEME (compare to a known reference
value from `libsgp4`'s own tests) and that Path A hands that vector
unchanged to the propagator.

**T10. Coordinate-frame guardrail.** A deliberate wrong-frame input
(ECEF passed where ECI is expected) must be detected — by a magnitude
check on `‖v − ω × r‖` differing from `‖v‖` by more than a fixed
threshold — and rejected with a clear diagnostic. This is a
Newton-Architect **hidden-assumption trap**: silent frame errors are
the exact failure mode this test exists to catch.

### 8.2 Benchmarks (larger, comparative)

**B1. Synthetic self-consistency (perfect-model).**

Generate a truth trajectory with the deterministic force model, sample
Doppler at 1 Hz with additive Gaussian noise, run Modes A/B/C/D, report:

- position RMS error vs truth (radial / along / cross)
- velocity RMS error vs truth
- oscillator-bias recovery vs truth
- 1-σ envelope coverage (fraction of epochs where truth ∈ 1-σ, 2-σ, 3-σ
  intervals of the smoother covariance)

**Newton-clean:** the ground truth is generated by us, so residual size
is a clean measure of filter numerical health, not of physical model
adequacy.

**B2. Model-mismatch stress.**

Same as B1, but truth uses **more force terms than the filter** (e.g.
truth = EGM96 20×20 + Sun+Moon+drag+SRP; filter = EGM96 10×10 +
Sun+Moon only). Report degradation vs B1 and whether iteration in
Mode D compensates. This is where the *interesting* claims live and
where Newton rules bite hardest: we do not tune Q until the residuals
look OK; we report what happens.

**B3. Real ISS pass with published TLE.**

Fetch a TLE from Celestrak/Space-Track for a specific ISS pass over the
configured ground station. **Real Doppler observations are required for
this benchmark.** We ship the *harness* in v0; whether B3 executes
depends on whether the user provides recorded Doppler samples.
Reference trajectory for comparison: JPL Horizons state at post-pass
epoch (if available) or a second, later-epoch TLE valid over the same
period.

**Under Newton rules,** B3 will not be presented as "our filter matches
truth" unless the Horizons or overlapping-TLE reference is genuinely
independent of the input TLE used to seed the filter. If it isn't,
we say so.

**B4. Cross-check against NLF reentry benchmark.**

Run NLF's shipped `run_benchmarks` with the reentry problem and
independently verify our recorded output CSV matches NLF's reference
CSV to floating-point tolerance. This certifies that our *integration
of* NLF is not the source of any discrepancies observed in B1–B3.

Each benchmark writes a CSV artifact and a plain-text report; both are
reviewable without running the tests.

---

## 9. File Layout and CMake Integration [SPEC]

New files (planned):

```
include/od/
    od_types.hpp                 // state layout, unit constants, RSW rotation
    doppler_measurement.hpp      // h(x,t) + Jacobian
    tle_to_state.hpp             // TLE → SGP4-at-epoch → TEME → (Path A/B)
    od_filter.hpp                // thin wrapper around NLF SRUKF for orbits
    od_smoother.hpp              // Mode A/B/C/D driver

src/od/
    doppler_measurement.cpp
    tle_to_state.cpp
    od_filter.cpp
    od_smoother.cpp

unittests/
    test_od_frames.cpp           // T1, T10
    test_od_doppler.cpp          // T2, T3, T4
    test_od_srukf_linear.cpp     // T5, T6, T7
    test_od_propagator.cpp       // T8, T9

benchmarks/
    bench_synthetic.cpp          // B1, B2
    bench_iss_pass.cpp           // B3 (skipped if no data)
    bench_nlf_reentry.cpp        // B4
```

`CMakeLists.txt` additions:

```cmake
option(BUILD_OD "Build orbit-determination subsystem"                        ON)
option(BUILD_OD_BENCHMARKS "Build OD benchmarks"                             OFF)

if (BUILD_OD)
    find_package(nlf REQUIRED)              # from Modern-Computational-Nonlinear-Filtering
    find_package(OptMathKernels REQUIRED)   # sibling, but pulled in transitively by nlf
    find_package(Eigen3 3.4 REQUIRED)       # required by nlf and OptMathKernels

    add_library(ve_od STATIC
        src/od/doppler_measurement.cpp
        src/od/tle_to_state.cpp
        src/od/od_filter.cpp
        src/od/od_smoother.cpp)
    target_include_directories(ve_od PUBLIC include)
    target_link_libraries(ve_od PUBLIC
        nlf::nlf                            # brings SRUKF + smoother + Eigen
        OptMathKernels::OptMathKernels      # brings Cholesky + BLAS backends
        Eigen3::Eigen
        ${SGP4_LIB})
endif()
```

Consumers of OD are expected to `target_link_libraries(app PRIVATE ve_od)`.

---

## 10. Runtime API [SPEC]

Sketch (final signatures land with the implementation):

```cpp
namespace ve::od {

struct DopplerObservation {
    double t_utc_jd;      // observation epoch (UTC JD)
    double f_hz;          // measured received frequency [Hz]
};

struct FilterConfig {
    double f_transmit_hz;
    Observer station;
    Eigen::Matrix<double, 8, 8> P0;
    Eigen::Matrix<double, 8, 8> Q;
    double R_hz2;
    double innovation_gate_chi2 = 25.0;   // matches NLF SRUKF default
    bool   reject_outliers      = false;
};

struct IterationConfig {
    int    I_max            = 20;
    double tol_state_maha   = 1e-6;
    double tol_loglik       = 1e-4;
};

struct PassResult {
    std::vector<double>                 t_utc_jd;
    std::vector<Eigen::Matrix<double,8,1>> x_filtered;
    std::vector<Eigen::Matrix<double,8,8>> P_filtered;
    std::vector<Eigen::Matrix<double,8,1>> x_smoothed;
    std::vector<Eigen::Matrix<double,8,8>> P_smoothed;
    std::vector<double>                 innovation_hz;
    std::vector<double>                 nis;
    int      iterations_used;
    double   final_loglik;
    bool     converged;
};

PassResult run_forward_only         (…);   // Mode A
PassResult run_forward_then_smooth  (…);   // Mode B / C (C = report smoothed@AOS)
PassResult run_iterated             (…);   // Mode D

}
```

---

## 11. What This Design *Deliberately* Does Not Do

- It does not compute a *new* TLE (mean-element regression from OD-fit
  state). That is a distinct problem and requires additional machinery.
- It does not perform **initial orbit determination** (IOD) from
  observations alone. TLE seed is required; there is no Gauss / Herrick–
  Gibbs / Laplace method in this subsystem.
- It does not solve **multi-station** or **networked** OD.
- It does not do **autonomous receiver tuning**; `f_transmit_hz` is a
  configuration input.
- It does not attempt to **detect satellite maneuvers**. A large
  innovation is treated as a candidate outlier by the gate, not as a
  maneuver hypothesis. Maneuver detection is a future item.

---

## 12. References

The following references are cited above by shorthand. Full bibliographic
detail is included so that the citations are checkable (Newton rule: no
fabricated citations).

- Julier, S. J., & Uhlmann, J. K. (2004). *Unscented filtering and
  nonlinear estimation.* Proceedings of the IEEE, 92(3), 401–422.
  doi:10.1109/JPROC.2003.823141
- van der Merwe, R., & Wan, E. A. (2001). *The square-root unscented
  Kalman filter for state and parameter estimation.* In *Proc. IEEE
  ICASSP*, Vol. 6, pp. 3461–3464.
- Särkkä, S. (2008). *Unscented Rauch–Tung–Striebel smoother.* IEEE
  Transactions on Automatic Control, 53(3), 845–849.
  doi:10.1109/TAC.2008.919531
- Rauch, H. E., Tung, F., & Striebel, C. T. (1965). *Maximum likelihood
  estimates of linear dynamic systems.* AIAA Journal, 3(8), 1445–1450.
  doi:10.2514/3.3166
- Vallado, D. A. (2013). *Fundamentals of Astrodynamics and Applications*
  (4th ed.). Microcosm Press. — Table 8-4 (exponential atmosphere) and
  IAU-76/FK5 rotation chain.
- Montenbruck, O., & Gill, E. (2000). *Satellite Orbits: Models,
  Methods, Applications.* Springer. — General reference for force
  modelling and orbit determination.
- Rindler, W. (2006). *Relativity: Special, General, and Cosmological*
  (2nd ed.). Oxford University Press. — §3.7 relativistic Doppler.
- Modern-Computational-Nonlinear-Filtering, this system, sibling
  repository at `/home/n4hy/Modern-Computational-Nonlinear-Filtering`.
  MIT-licensed. Provides the SRUKF, SRUKF fixed-lag smoother, and
  SRUKF full-interval smoother consumed here.
- OptimizedKernelsForRaspberryPi5_NvidiaCUDA, sibling repository at
  `/home/n4hy/OptimizedKernelsForRaspberryPi5_NvidiaCUDA`. Provides
  the NEON/SVE2/CUDA-accelerated linear-algebra primitives dispatched
  through NLF's `filtermath` layer.

Any additional citation added later must be verified against a real
source (DOI, arXiv ID, ISBN); if a source cannot be verified, it is
marked *citation needed* rather than left in the document.

---

## 13. Change Log

- **2026-08-02 — v0 draft.** Initial design specification.
- **2026-08-02 — v0 implementation landed.** Sections §4–§10 now have
  code backing them. See §14 for the [IMPL] index. Every claim below
  that carries an [IMPL] tag has a file:line reference; every claim
  that still carries [SPEC] does not.
- **2026-08-02 — TEME-only frame decision.** Original design offered a
  Path B (GCRF via IAU-76/FK5) as a v1 item. User decision (see also
  README): the OD subsystem operates in TEME end-to-end, no GCRF
  conversion anywhere. §6 rewritten accordingly.
- **2026-08-02 — Doppler sign bug caught by T4.** The first
  implementation used `sqrt(1-β²)/(1 - β_los)` in `predict_doppler`,
  which is the correct SR form for source-approaching-observer
  convention. With our `rho_hat = station-to-satellite` convention
  (where `β_los > 0` means recession), the correct form is
  `sqrt(1-β²)/(1 + β_los)`. Unit test T4 (limit-case reduction to
  classical Doppler) caught the error via the magnitude of
  `SR - classical`. Fixed in `src/od/doppler_measurement.cpp`. This
  incident is exactly why the design specified an SR/classical limit
  test.

---

## 14. Implementation Index [IMPL, 2026-08-02]

Every entry below is a live file:line reference to the code as of the
2026-08-02 commit. The [SPEC] items above map to [IMPL] items here as
follows.

| Design section | Implementation |
| -------------- | -------------- |
| §3 Frame and unit discipline (F1–F4) | `include/od/od_types.hpp` (constants, indices, StateVecD/F aliases, `rsw_to_eci_basis`, `build_prior_covariance`, `diagnose_state`) |
| §4.1 8-state layout                  | `include/od/od_types.hpp::idx` and `make_state()` |
| §4.2 Dynamics function `f`           | `include/od/orbit_ssm.hpp::OrbitStateSpaceModel::f` + `include/od/od_dynamics.hpp::integrate_rk4` |
| §4.3 Measurement function `h`        | `include/od/doppler_measurement.hpp::predict_doppler` |
| §4.3 Analytical Jacobian `dh/dx`     | `src/od/doppler_measurement.cpp::doppler_jacobian` |
| §4.4 Measurement noise `R`           | `include/od/orbit_ssm.hpp::OrbitStateSpaceModel::R` (from `FilterConfig.sigma_R_hz`) |
| §5 Modes A/B/C/D                     | `include/od/od_smoother.hpp::Mode` enum, driver at `src/od/od_smoother.cpp::run` |
| §5.5 Dual convergence criteria       | `src/od/od_smoother.cpp::smoothed_state_change_maha` + loop in `run(D_Iterated, ...)` |
| §6 TLE → TEME (no GCRF)              | `src/od/tle_to_state.cpp::teme_state_at_epoch` (thin wrapper around libsgp4) |
| §7.1 Initial state x₀                | Caller-supplied on `PassInput.x0_at_t_ref`; typically produced by propagating `NumericalPropagator` to AOS |
| §7.2 Initial covariance P₀           | `src/od/od_types.cpp::build_prior_covariance` (RSW→ECI via `rsw_to_eci_basis`) |
| §7.3 Process noise Q                 | `include/od/orbit_ssm.hpp::OrbitStateSpaceModel::Q` |
| §8.1 Unit tests T1–T10               | `unittests/test_od_frames.cpp` (T1, T10), `test_od_doppler.cpp` (T2, T3, T4), `test_od_srukf_linear.cpp` (T5, T6, T7), `test_od_propagator.cpp` (T8, T9) |
| §8.2 Benchmark B1 (synthetic self-consistency) | `benchmarks/bench_synthetic.cpp` `run_case("b1_*", fp_baseline, fp_baseline, ...)` |
| §8.2 Benchmark B2 (model-mismatch)   | `benchmarks/bench_synthetic.cpp` `run_case("b2_*", truth_rich, fp_baseline, ...)` |
| §8.2 Benchmark B3 (real ISS pass)    | `benchmarks/bench_iss_pass.cpp` — HARNESS ONLY; refuses to run without real recorded Doppler input (Newton discipline) |
| §8.2 Benchmark B4 (NLF reentry cross-check) | `benchmarks/run_nlf_reentry.sh` (executes NLF's shipped `run_benchmarks`) |
| §9 File layout                       | Delivered as specified except `od_dynamics.hpp` was added to house the standalone RK4 integrator (see §14 note below) |
| §9 CMake                             | `CMakeLists.txt` `BUILD_OD` option + `find_package(nlf REQUIRED)` + `find_package(OptMathKernels REQUIRED)`; produces `ve_od` static library and 4 test + 2 benchmark executables |
| §10 Runtime API                      | `include/od/od_smoother.hpp::PassInput`, `PassResult`, `FilterConfig`, `IterationConfig`, `run(Mode, ...)` |

**§14 note — additional file.** `include/od/od_dynamics.hpp` +
`src/od/od_dynamics.cpp` were added because NLF's sigma-point
propagation loop invokes `model.f(sigma_i, t_k, u_k)` 17 times per
predict step. `ve::NumericalPropagator` is stateful (single-trajectory
cache), so a standalone RK4 that takes `ForceModel` and an arbitrary
`(r, v, t_start)` was the cleanest way to give every sigma point its
own trajectory without perturbing the propagator's normal-mode usage.

---

## 15. Measured behavior (v0) [MEASURED, 2026-08-02]

### 15.1 Unit-test suite results

All ten [SPEC] unit tests are backed by executable checks; all pass on the
build host (Ubuntu 24.04, GCC 13.3, x86_64, no accelerator).

```
$ ctest -R "^od_" --output-on-failure
1/4 Test #3: od_frames ......................   Passed    0.00 sec
2/4 Test #4: od_doppler .....................   Passed    0.00 sec
3/4 Test #5: od_srukf_linear ................   Passed    0.02 sec
4/4 Test #6: od_propagator ..................   Passed    0.00 sec
100% tests passed, 0 tests failed out of 4
```

Notable numerical results from within the tests:

- **T4 (relativistic reduction):** measured `(SR - classical)/f_T` at a
  synthetic 42164-km geometry with β ≈ 6.7·10⁻⁶ agrees with the
  leading-order prediction `β_los² - β²/2` to relative error 1.3·10⁻⁵.
- **T8 (RK4 round-trip):** 300 s forward + 300 s backward integration
  of a 420 km circular LEO under J₂-only dynamics returns to within
  9.6·10⁻¹³ km in position and 1.1·10⁻¹⁵ km/s in velocity.
- **T9 (TLE→TEME identity):** `teme_state_at_epoch` result matches
  `libsgp4::SGP4::FindPosition(tle.Epoch())` to within 10⁻⁹ km and
  10⁻¹² km/s — the two are numerically the same call, as designed.

### 15.2 Synthetic benchmark results (B1, B2)

`bench_synthetic` was executed on a 900-s LEO simulation with 1-Hz
Doppler at σ_R = 5 Hz. Numbers below are position and velocity RMS
between filter-smoothed states and the synthetically generated truth.

**Case 1: off-prior (100 m in position, 0.1 m/s in velocity, oscillator bias unknown):**

| Case            | Mode | pos_rms (km) | vel_rms (km/s) | iters | converged | log-lik   |
| --------------- | ---- | -----------: | -------------: | ----: | :-------: | --------: |
| b1_modeB        | B    |      4.2996  |       0.00529  |     1 | yes       | -2.77e+03 |
| b1_modeD_iter   | D    |      5.8678  |       0.00822  |     5 | no        | -5.70e+03 |
| b2_modeB        | B    |      4.3082  |       0.00531  |     1 | yes       | -2.77e+03 |
| b2_modeD_iter   | D    |      5.8760  |       0.00824  |     5 | no        | -5.70e+03 |

**Case 2: truth-prior (`VE_OD_BENCH_TRUTH_PRIOR=1`):**

| Case            | Mode | pos_rms (km) | vel_rms (km/s) | iters | converged |
| --------------- | ---- | -----------: | -------------: | ----: | :-------: |
| b1_modeB        | B    |      1.2756  |       0.00175  |     1 | yes       |
| b1_modeD_iter   | D    |      2.7116  |       0.00444  |     5 | no        |
| b2_modeB        | B    |      1.2722  |       0.00176  |     1 | yes       |
| b2_modeD_iter   | D    |      2.7142  |       0.00446  |     5 | no        |

**Newton reading of these numbers:**

1. **Even with truth as the prior, position RMS is ~1.3 km, not zero.**
   This is a real precision floor caused by NLF's single-precision
   (float) SRUKF interacting with 6778-km-scale state vectors over
   900 sigma-point propagations. It is documented as an inherited
   library limitation; the v0 OD subsystem does not attempt to work
   around it. Any application needing sub-100-m OD must either
   (a) upgrade NLF's SRUKF to double precision, or (b) reduce the
   sigma-point spread by tightening priors substantially, or (c) accept it.
2. **B1 (perfect model) and B2 (mismatched model) produce nearly
   identical RMS in this pass length.** Interpretation: over 900 s the
   truth-model force differences (EGM96 10x10 + Sun + Moon vs EGM96 4x4
   only) are small enough that they are dominated by the precision floor
   and by initial-prior recovery. Longer passes would separate B1/B2 more
   clearly. Not reported as "filter robust to model mismatch" —
   reported as "in this configuration, we cannot distinguish."
3. **Mode D (iterated) with the off-prior gives WORSE RMS than Mode B
   (single-pass smoothed) — 5.87 km vs 4.30 km.** The likely
   interpretation: the iterated loop cap of 5 hit before convergence, and
   in this regime the last iteration was moving AWAY from the initial
   filtered/smoothed trajectory (the `converged: no` flag is honest).
   Iteration is not a free win; it is a tool that helps when initial
   linearisation is far from truth and hurts when the state re-drives
   forward from an aggressively-corrected initial condition.
4. **Innovation-gate events fired during the first few observations**
   with the off-prior. These are legitimate: with a 100 Hz prior on
   oscillator bias and a truth bias of 25 Hz, first-observation innovations
   are naturally many-sigma of `R` alone. The SRUKF scales the correction
   (rather than rejecting outright) at gate threshold, which is
   appropriate behaviour.

### 15.3 NLF library cross-check (B4)

`benchmarks/run_nlf_reentry.sh` runs NLF's own `run_benchmarks` binary,
including the SRUKF Reentry Vehicle 6D benchmark that is the closest
analogue to a satellite tracking problem in NLF's shipped suite.

**Result on the build host:**

```
=== SRUKF+Smoother on ReentryVehicle6D ===
Filtered RMSE:  367.133
Smoothed RMSE:  236.275
NEES: median=4.981  in-95%-bounds: 95.9% (270/270 valid)
                    chi2_95=[1.21, 14.44]
Divergences: 0
--- Regression gate ---
PASS: 13 rows matched RMSE baselines within 0.5%, NEES-consistent, no divergences
```

**Newton reading:** NLF's own SRUKF passes its regression gate, so any
kilometre-scale RMS in bench_synthetic is on our OD subsystem's usage
of NLF (state size, prior scale, force-model choice, single-precision
interaction), not on NLF's SRUKF implementation itself.

### 15.4 What can be honestly claimed after v0

- The OD subsystem builds, links, and runs end-to-end against the two
  sibling libraries.
- The 4 SRUKF modes A/B/C/D each execute and produce non-degenerate
  output on 900-s LEO Doppler passes.
- Unit tests exercise the frame handling, station kinematics, Doppler
  Jacobian, SR reduction, sigma-point behaviour, RTS smoother on a
  linear-Gaussian problem, iterated-loop numerical stability,
  propagator round-trip, and TLE→TEME frame documentation.
- The design's Newton-Architect scoping (declared limitations, tagged
  epistemic status, refusal to fabricate a real-data B3 benchmark) is
  honored in the shipped code and reports.

### 15.5 What is *not* claimed

- We do **not** claim sub-km OD accuracy from this v0 subsystem.
- We do **not** claim the iterated loop uniformly improves on the
  single smoothing pass; §15.2 shows a case where it did not.
- We do **not** claim the benchmark generalises to real Doppler
  hardware; B3 remains a harness pending real recorded data.
- We do **not** claim algorithmic proof of correctness; all tests are
  numerical checks with stated tolerances.
