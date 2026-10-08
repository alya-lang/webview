{{DESCRIPTION}}

## 📦 Installation

Using the Alya CLI:

```bash
alya add {{PACKAGE_NAME}} --git https://github.com/{{REPOSITORY}} --tag {{TAG}}
alya install
```

Or add it directly to your project's `alya.toml`:

```toml
[dependencies]
{{PACKAGE_NAME}} = { git = "https://github.com/{{REPOSITORY}}", tag = "{{TAG}}" }
```

## 🚀 What's Changed

{{CHANGELOG_COMMITS}}

## 🔗 Resources

- **Documentation**: [README.md](https://github.com/{{REPOSITORY}}#readme)
- **Examples**: [examples/](https://github.com/{{REPOSITORY}}/tree/{{TAG}}/examples)
- **Issue Tracker**: [GitHub Issues](https://github.com/{{REPOSITORY}}/issues)

---

{{FULL_CHANGELOG}}
