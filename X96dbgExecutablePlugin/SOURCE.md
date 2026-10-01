# Corresponding source

- KSword launcher, adapter and core integration sources: the KSword repository
  at the revision used to build this package. See its source archive for local
  changes that may not yet be published.
- x64dbg audited core: `https://github.com/x64dbg/x64dbg`, commit
  `f107330b6563da3c38d60a3ad6e629057b7cf5d0`, with the KSword engine selection
  patch distributed with this project.
- Native TitanEngine: `https://github.com/x64dbg/TitanEngine`, commit
  `21ef77f31fd42d17f785802c36b2ca4ca44c43d7`.

The generated runtime manifest records hashes of the actual supplied binaries
and the selected canonical engine ABI. A source commit alone is not proof that
an arbitrary downloaded binary implements the current engine ABI.
