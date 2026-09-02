# Kotlin Grammar for Tree-sitter

This crate provides Brokk's maintained package of the
[`fwcd/tree-sitter-kotlin`](https://github.com/fwcd/tree-sitter-kotlin) grammar.
It prefixes all native symbols so it can coexist with another Kotlin grammar
crate in one executable.

To use this crate, add it to the `[dependencies]` section of your `Cargo.toml` file:

```toml
tree-sitter = "0.24"
brokk-tree-sitter-kotlin = "=0.4.3"
```

Typically, use the `LANGUAGE` constant with a tree-sitter [`Parser`](https://docs.rs/tree-sitter/*/tree_sitter/struct.Parser.html):

```rust
let code = r#"
  data class Point(
    val x: Int,
    val y: Int
  )
"#;
let mut parser = tree_sitter::Parser::new();
parser
    .set_language(&brokk_tree_sitter_kotlin::LANGUAGE.into())
    .expect("Error loading Kotlin grammar");
let parsed = parser.parse(code, None);
```
