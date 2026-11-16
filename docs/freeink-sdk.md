# FreeInk SDK changes for MicroSlate

`freeink-sdk` is a submodule pointing at MicroSlate's fork of the FreeInk SDK,
`Josh-writes/freeink-sdk`, branch `microslate-e1001`. That branch is the upstream
`Free-Ink/freeink-sdk` commit `111fdcc` plus one commit with the Seeed reTerminal E1001
support (board profile, UC8179 7.5" panel driver, driver selection). It used to be carried
here as `freeink-sdk-e1001.patch`.

A normal clone builds every device:

    git clone --recursive <this repository>

To take a newer upstream SDK: fetch `Free-Ink/freeink-sdk`, rebase `microslate-e1001` onto it,
push the branch to the fork, then commit the new submodule pointer here.
