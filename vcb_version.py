# Version of the recovered VCB simulation engine in `modules/vcb/`.
#
# This is deliberately separate from Godot's own `version.py`: the engine half of
# this repo is stock Godot 3.5.1 and only `modules/vcb/` is ours, so only
# `modules/vcb/` gets versioned here. Release tags splice the two together:
#
#     <godot major>.<minor>.<patch>-vcb-<major>.<minor>.<patch>
#     e.g. 3.5.1-vcb-1.0.0
#
# Bumping the numbers below and pushing to `master` is what triggers a release
# (see `.github/workflows/release.yml`). Nothing else reads this file — it is not
# compiled into the binary, because `modules/vcb/` must stay byte-identical to the
# vendored copy in `vcb-rebuild` and adding a header there would break that.
#
# Rough intent, matching how the module is actually developed:
#   major -- the compiled circuit graph or the tick kernel changed observably
#            (a board that used to simulate one way now simulates another).
#   minor -- new classes/methods, new verified behaviour, no regression for
#            existing boards.
#   patch -- fixes that move the module closer to the original `vcb.exe`, and
#            anything invisible to a board author.

major = 1
minor = 0
patch = 0
