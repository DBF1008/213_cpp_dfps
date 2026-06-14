// Stub for host-side regression build. The dfps source includes <scn/scn.h>
// and <spdlog/spdlog.h> but the subset we exercise in the deadlock test
// (ExecCmdSync, ParseInt, ...) does not reference either. Real device builds
// still link the genuine libraries via CMake.
#pragma once
