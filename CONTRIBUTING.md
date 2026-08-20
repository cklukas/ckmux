# Contributing to ckmux

ckmux is licensed under the [MIT License](LICENSE). Contributions are
welcome, under the terms below — they exist so that what you may do with
this project, and what this project may do with your contribution, is
written down rather than assumed.

## Contribution terms

By submitting a contribution to this repository you agree that:

1. **License.** Your contribution is licensed under the MIT License, the
   same license this project is distributed under. This writes down the
   "inbound = outbound" custom instead of relying on it.
2. **Patent grant.** You grant the project, its maintainer, and every
   recipient of this software a perpetual, worldwide, non-exclusive,
   royalty-free, irrevocable patent license to make, use, sell, offer for
   sale, import, and otherwise transfer your contribution alone and in
   combination with this project, covering any patent claims you control
   that are necessarily infringed by your contribution.
3. **Relicensing.** You grant the project owner (Dr. Christian Klukas) the
   right to relicense your contribution, alone or as part of this project,
   under other license terms, including commercial terms. The public MIT
   license of anything already released is never revoked by this — it
   exists so the project can also be offered under additional terms without
   tracking down every past contributor.

If you cannot agree to these terms, do not submit the contribution.

## Developer Certificate of Origin

Every commit must carry a `Signed-off-by:` line with your real name and a
working email address (`git commit -s`). The sign-off certifies the
[Developer Certificate of Origin 1.1](https://developercertificate.org/):
that you wrote the contribution, or otherwise have the right to submit it
under this project's license. Commits without a sign-off are not merged.

## Provenance (binding — read before writing a line)

This project has a strict provenance rule, and contributions are held to it
without exception: **never port, transcribe, or consult the source code of
tmux, GNU screen, zellij, dtach/abduco, mtm, Twin, Turbo Vision, or any port
or derivative of them** — not for a constant, not for a terminfo string, not
for anything. Behavior is derived only from published standards (ECMA-48,
xterm ctlseqs, the kitty protocol specs, Unicode UAX #11/#29, terminfo(5),
POSIX), from ckVision's own documentation, and from documented black-box
observation of terminals. Their manuals are fine; their source is not. A
contribution that cannot honestly state this provenance will not be merged,
however good it is.

## The practical bar

Every change lands with a test that fails without it. The build is
zero-warning under `-Wall -Wextra -Wpedantic -Wshadow -Werror` and green
under ASan/UBSan. Documentation is updated in the same commit when behavior
changes. Commit messages carry real verification evidence — what you ran and
what it showed — not "all tests pass".
