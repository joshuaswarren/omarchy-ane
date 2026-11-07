# H17 PS table: four real entries, not a duplicated fifth

Parent `14790e1`. Replace the duplicate c010 fifth slot introduced in
`8c2af07` with an explicit per-SoC valid prefix count. The T8140 row
contains 290/c000/c008/c010 and ps_count=4. Legacy rows default to five
slots, unchanged. H17 sub-island role names remain unassigned and are
printed as SUB0/SUB1/SUB2 rather than borrowed H16 roles.

The simulator's pos-four-ps test leaves the fifth slot off and verifies
only four words are read twice; an excessive count is rejected before
ioremap/runtime power. The full simulator/mutation suite and W=1 kernel
build pass. This change was not exercised on ANE hardware; the T8140
stage=boot early refusal remains unchanged.

Private full command outputs:
`/var/tmp/opencode/h17-ps-count-sim.log` and
`/var/tmp/opencode/h17-ps-count-build.log`.
