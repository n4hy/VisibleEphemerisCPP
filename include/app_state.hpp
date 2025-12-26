#pragma once

namespace ve {
    enum class AppState {
        OPTICAL_TRACKING, // Standard mode: Filter by visual visibility constraints
        RADIO_TRACKING,   // Radio mode: Show everything above horizon (ignore shadow)
        BUILDER_MODE      // Mission Planner mode
    };
}
