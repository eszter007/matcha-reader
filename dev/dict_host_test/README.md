# Dictionary lookup host test (desktop-only dev tool)

Not part of the firmware build. Exercises `DictIndex`, `Deinflector`, and
`WordLookup` against a tiny synthetic dictionary on your laptop, without
flashing a device. Uses stub headers for `HalStorage` and `Logging` that
wrap standard stdio.

## Run it

From the repo root:

```
cd dev/dict_host_test
g++ -std=c++20 \
  -I stubs -I ../../lib/Dict -I ../../lib/Memory \
  -o dtest \
  ../../lib/Dict/Deinflector.cpp \
  ../../lib/Dict/DictIndex.cpp \
  ../../lib/Dict/WordLookup.cpp \
  main_test.cpp
./dtest
```

The test builds a synthetic `jmdict.idx`/`.dat` pair at `/dict/` with a
handful of entries, then runs four test groups:

1. **DictIndex exact lookup** -- binary search over the sorted index
2. **Deinflector** -- ichidan/godan/irregular verb and i-adjective deinflection
3. **WordLookup end-to-end** -- sliding-window lookup over a paragraph
4. **Edge cases** -- empty input, out-of-bounds offset, single-char headword
