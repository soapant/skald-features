# skald-features

Features for [Skald](https://github.com/soapant/skald) that I use daily in my
own workflow. Skald ships with no widgets or remaps, so this repo is both a
set of working examples and a reference for writing your own.

> ⚠️ **Unstable.** These features are written against Skald's current,
> unstable API (`skapi.h`) and can break when it changes.

> **Read before you install.** A feature is compiled code that runs inside
> Skald with your user's permissions. It can read input, the clipboard, and
> files. Read a feature's source before installing it.

## Installing a feature

Manual for now. Copy the feature's folder into your `FeaturePath`, then reload
features. See the Skald README for details. You need a C compiler (temporary,
until features are easier to install).

## Contributing

Have a useful feature? Open a PR and I might take a look. There are no
promises or timeline. Put each feature in its own folder with its `.c`, a
`.toml` manifest is automatically generated,See [CONTRIBUTING.md](CONTRIBUTING.md).

## License
MIT
