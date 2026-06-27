# The Ouroboros Markdown Viewer

The first real application on Object RISC -- this document is read off the host filesystem, parsed, and rendered live through the window manager's font service.

## Features

- Proportional Lucida Sans body text, wrapped to the window width
- Bold headings, hairline rules, and bulleted lists
- Monospace code blocks on a tinted panel

The viewer opens this very file with a single call:

```
int fd = hf_open("/docs/welcome.md", 0);
```

All of it runs on an OPEN LOOK desktop -- down to the scrollbar, which will soon scroll this text for real.
