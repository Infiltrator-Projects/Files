# Files 0.1.17

Files 0.1.17 hardens destination ownership when file operations race with other namespace writers.

The release keeps directory and symbolic-link publication inside descriptor-rooted protected staging, verifies owned objects before destructive cleanup, preserves competing-writer data, reports retained source data explicitly when safe restoration is blocked, and fails closed with a typed unsupported-capability result when a provider cannot supply atomic no-replace semantics.

Release-effective adversarial coverage includes staging-container replacement, quarantine cleanup replacement and blocked source restoration, in addition to the existing copy, move, rename, replacement, cancellation and recovery qualification.
