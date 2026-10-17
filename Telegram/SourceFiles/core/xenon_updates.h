#pragma once

namespace Core::XenonUpdates {

[[nodiscard]] bool Supported();
[[nodiscard]] bool AutomaticallyCheck();
[[nodiscard]] bool AutomaticallyInstall();
void SetAutomaticallyCheck(bool enabled);
void SetAutomaticallyInstall(bool enabled);
void Start(bool scheduled = false);
void CheckNow();
void ForceUpdate();
[[nodiscard]] rpl::producer<QString> Status();
[[nodiscard]] QString BuildDescription();

} // namespace Core::XenonUpdates
