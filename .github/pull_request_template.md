## Summary

Describe the user-visible behavior and the narrow implementation scope.

## Validation

- [ ] Relevant unit and integration tests pass.
- [ ] Public C headers compile as C11.
- [ ] Public C++ headers compile as C++23.
- [ ] `git diff --check` passes.
- [ ] Compatibility golden files were not changed to hide a regression.
- [ ] New third-party code includes provenance and license information.

## Safety

- [ ] Destructive Fastboot behavior is covered by a fake-device or HIL test.
- [ ] Cancellation and unknown-delivery outcomes do not retry automatically.
