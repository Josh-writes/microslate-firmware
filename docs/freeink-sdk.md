# FreeInk SDK for MicroSlate

`freeink-sdk` is a submodule pointing at upstream `Free-Ink/freeink-sdk`, branch `main`.
MicroSlate's E1001 support (board profile, UC8179 7.5" panel driver, sample env, docs, host test)
was merged upstream as Free-Ink/freeink-sdk#224, so MicroSlate no longer needs its own fork.

This FreeInk also drives the newer Sticky's SSD2677 screen (detected at boot). MicroSlate 3.0.4
and earlier used the old fork branch `Josh-writes/freeink-sdk` `microslate-e1001`.

A normal clone builds every device:

    git clone --recursive <this repository>

To take a newer SDK: in `freeink-sdk`, fetch and check out the wanted upstream `main` commit,
build and device-test, then commit the new submodule pointer here.
