# Coding conventions

- Use descriptive names of at least three characters for pointer variables and function parameters.
- Put each executable statement on its own line.
- Always use braces for `if`, `for` and `while` bodies, including single statements. Use braced `else` bodies while preserving normal `else if` chains. Follow the Linux brace style configured in `.clang-format`.
- Declare one variable per declaration, including fields in structs and unions. Preserve types, qualifiers, field order and initializers when splitting existing declarations.
- Group multiple Boolean members of runtime structures into adjacent C99 `bool : 1` fields. Preserve natural alignment of other members and explicit alignment of backend sector buffers. Never use bit-field layout as an on-disk representation.
- For runtime layout optimizations, group members by required alignment and keep byte-sized flags and odd-byte arrays near the end. Preserve mandatory sub-operation prefixes and aligned scratch storage. Never reorder fixed disk or external-protocol records. Measure before/after memory in the affected ABIs.
- Preserve the library's asynchronous cache and state-machine structure. Style changes must not change behavior or increase memory usage.
