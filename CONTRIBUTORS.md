# Contributors

Thank you to everyone who has helped make HomRec better - through code,
bug reports that turned into fixes, docs, translations, or plugins.

## Maintainer

- **[homaaio](https://github.com/homaaio)** - creator and maintainer.
  Contact: see [SUPPORT.md](SUPPORT.md).

## Contributors

<!--
  Add yourself here in the same PR as your contribution (or ask in the
  PR/Issue and the maintainer will add you). One line per person, in
  the order added:

  - [@github-handle](https://github.com/github-handle) - short summary
    of what you worked on (e.g. "fixed the QSV encoder freeze on Intel
    UHD Graphics", "German translation (.hrl)", "bter plugin").
-->

- *(this project is looking for its first outside contributor - see
  below!)*

## How this file works

This is a simple thank-you list, not a legal record - HomRec doesn't
require a CLA (Contributor License Agreement) or copyright assignment.
By opening a pull request you're agreeing to license your contribution
under the same terms as the rest of the project (GPLv3, see
[LICENSE](LICENSE)).

**To get listed:** open a pull request and add a line for yourself under
"Contributors" above, in the same PR as your actual change. If you'd
rather not do the Markdown yourself, just say so in the PR description
and the maintainer will add you when it's merged.

**What counts:** anything that took real effort and made it into the
project - a code change, a translation (`.hrl` file), a plugin bundled
in `Hom/plugins/`, a documentation fix, or a bug report detailed enough
that it directly led to a fix. Typo fixes and one-line nits are welcome
too, just less likely to need a whole changelog line of their own -
they'll still get you listed if you'd like.

**What doesn't count on its own:** opening an Issue with no further
follow-through, or a PR that doesn't get merged. That's still useful
and appreciated, just not what this particular file tracks.

## Want to contribute?

There's no separate CONTRIBUTING.md yet - for now:

1. For code changes, see the "Build from source" section of
   [README.md](README.md#option-b---build-from-source) to get a local
   build working first.
2. Check [SUPPORT.md](SUPPORT.md) for how bug reports and feature
   requests are handled, and open an Issue before a large PR so the
   approach can be agreed on first - HomRec's whole design goal is
   staying light on CPU/RAM for weak PCs, so changes that trade that
   away for features need discussion up front.
3. Plugins don't need a PR to this repo at all - see
   [plugins/READMEplugins.md](plugins/READMEplugins.md) and
   [Hom/README.md](Hom/README.md) for publishing one independently
   through `hom`, HomRec's plugin package manager.
