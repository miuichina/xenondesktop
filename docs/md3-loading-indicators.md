# Material 3 circular loading indicators

The Xenon toggle `xenon-md3-loading-indicators` selects circular progress
motion from AndroidX Material 3, with the track treatment used by Xenon
Android's `MediaActionDrawable.java`.

Sources:

- [Google AndroidX ProgressIndicator.kt](https://android.googlesource.com/platform/frameworks/support/+/dd97834aa54671ee1f56d65fa46668b4ffeb57e8/compose/material3/material3/src/commonMain/kotlin/androidx/compose/material3/ProgressIndicator.kt)
- Xenon Android `org/telegram/ui/Components/MediaActionDrawable.java`, whose
  source attribution names AndroidX revision
  `606942979fae7ceb4ac1d0207aef25609b1b0c6c`.

The indeterminate cycle lasts 6000 ms. A linear global rotation covers
1080 degrees, with four additional 90 degree rotations. Each additional
rotation takes 300 ms and starts every 1500 ms. The active arc grows linearly
from 10% to 87% during the first 3000 ms, then shrinks using cubic Bezier
`(0.2, 0, 0, 1)`. The easing implementation solves the Bezier's x coordinate
before evaluating its y coordinate, matching the Android implementation.

The second arc is the remaining track, not an independently animated segment.
As in Xenon Android it uses 40% of the active color opacity and 22 degree
gaps at both ends. The gaps merge as determinate progress approaches zero
or completion. Known progress starts at the top and does not rotate.

`RadialAnimation` and `InfiniteRadialAnimation` share the drawing helper.
Consumers that draw computed states directly also call that helper:
proxy checks, emoji sets, emoji buttons, QR login, downloads, profile previews,
group-call sending, poll sending and transcription buttons. The cross-button
loading animation uses the same motion. Existing colors, sizes and disabled
animation rendering belong to each host widget.

The Android example's optional wavy path is controlled by
`xenon-md3-loading-waves`, shown below the MD3 toggle. It uses eleven waves,
an amplitude of 5% of the radius and one wavelength of travel per second.
The active segment follows the path by distance, while the track stays circular.
Both toggles must be enabled for waves to appear.

For known progress, amplitude is active between 10% and 95%. Outside that
range it fades to zero over 500 ms using `(0.3, 0, 0.8, 0.15)`; it fades in
using `(0.2, 0, 0, 1)`, matching Xenon Android. A change of target starts
from the currently displayed amplitude.

For unknown progress, the wave fades out over 500 ms when the active arc
starts shrinking at 3000 ms and the background arc starts expanding.
It fades back in over the first 500 ms of the next growth phase.

File, photo, GIF and video indicators have an additional style-scaled 2 px
inset when MD3 is enabled, including grouped media, the media viewer and PiP.
The ordinary indicator retains its original dimensions.
