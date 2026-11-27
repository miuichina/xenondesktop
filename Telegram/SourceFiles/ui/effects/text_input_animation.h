#pragma once

namespace Ui {
class InputField;

namespace TextInputAnimation {

enum class Mode { Blur, Explosion, Slide };
enum class Phase { Appearance, Removal, BlurAppearance };
enum class Parameter {
	Cursor, Fade, Blur, BlurDuration,
	Force, Gravity, Bounce, Spin, Shards, LifetimeMin, LifetimeMax,
	Direction, SlideAngle, DistanceMin, DistanceMax, SlideDuration,
};

struct ParameterSpec {
	const char *key;
	int min;
	int max;
	int step;
	int initial;
	QString unit;
};

[[nodiscard]] ParameterSpec Spec(Parameter parameter);
[[nodiscard]] QString Title(Parameter parameter);
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);
[[nodiscard]] Mode CurrentMode();
void SetMode(Mode mode);
[[nodiscard]] int Value(Parameter parameter);
void SetValue(Parameter parameter, int value);
[[nodiscard]] bool Disabled(Mode mode, Phase phase);
void SetDisabled(Mode mode, Phase phase, bool disabled);
[[nodiscard]] bool Fall();
void SetFall(bool enabled);
[[nodiscard]] bool IgnoreFloor();
void SetIgnoreFloor(bool enabled);
[[nodiscard]] rpl::producer<> Changes();
void Attach(not_null<InputField*> field);
void Install();

}
}
