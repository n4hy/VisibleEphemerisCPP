// od_types.cpp -- Definitions for helpers declared in od_types.hpp that are
// not header-only.

#include "od/od_types.hpp"

namespace ve::od {

StateMatD build_prior_covariance(const ve::Vector3& r,
                                 const ve::Vector3& v,
                                 const RswPriorSigmas& s) {
    // Position block in RSW basis.
    Eigen::Matrix3d P_r_rsw = Eigen::Matrix3d::Zero();
    P_r_rsw(0,0) = s.sigma_r_radial * s.sigma_r_radial;
    P_r_rsw(1,1) = s.sigma_r_along  * s.sigma_r_along;
    P_r_rsw(2,2) = s.sigma_r_cross  * s.sigma_r_cross;
    Eigen::Matrix3d P_v_rsw = Eigen::Matrix3d::Zero();
    P_v_rsw(0,0) = s.sigma_v_radial * s.sigma_v_radial;
    P_v_rsw(1,1) = s.sigma_v_along  * s.sigma_v_along;
    P_v_rsw(2,2) = s.sigma_v_cross  * s.sigma_v_cross;

    // Rotate to ECI (TEME).
    const Eigen::Matrix3d M = rsw_to_eci_basis(r, v);
    const Eigen::Matrix3d P_r_eci = M * P_r_rsw * M.transpose();
    const Eigen::Matrix3d P_v_eci = M * P_v_rsw * M.transpose();

    StateMatD P = StateMatD::Zero();
    P.block<3,3>(idx::RX, idx::RX) = P_r_eci;
    P.block<3,3>(idx::VX, idx::VX) = P_v_eci;
    P(idx::BC, idx::BC) = s.sigma_bc * s.sigma_bc;
    P(idx::BD, idx::BD) = s.sigma_bd * s.sigma_bd;
    // Newton note: no explicit r-v covariance. The forward filter builds any
    // legitimate r-v cross-correlation from the dynamics; injecting one here
    // without a derivation would be an unjustified assumption.
    return P;
}

} // namespace ve::od
