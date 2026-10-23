# Garage menu/save crash — 2026-10-23

The reported crash followed wheel-rim browsing and the SavingScreen transition. The guest stack was
`EA9F0 -> EA950 -> 1B3C00 -> 1B49E0`, with a read at `0x00740064` while freeing a string-table entry.

`EA750` originally gives each entry its own UTF-16 allocation. Extended garage labels replaced the owned text
pointer with an interior slice of a shared arena. `EA950` then passed that slice to the game's ordinary delete,
which interpreted UTF-16 text as heap metadata. The original placeholder allocation also leaked.

The hook now allocates an independent guest string for each replacement, releases the old owned placeholder,
and invalidates its table cache at the actual destructor boundary. Reopening a table at a reused address can
populate labels again. Allocation failure leaves the old placeholder intact and permits a later retry.

`tests/garage_strings_fixture.cpp` executes the actual generated `EA950` and `EA9F0` bodies. Three teardown/reopen
cycles, address reuse, allocation failure/retry and translated label preservation pass. Substituting the previous
arena-pointer assignment reproduces the interior free and exits with the expected failure code 73.

The updated native build and all 18 C++ regression checks pass. Live customization/save retesting follows the
build; this regression does not claim every purchase, profile or car has been tested.
