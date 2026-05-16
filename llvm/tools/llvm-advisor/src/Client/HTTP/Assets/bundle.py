#!/usr/bin/env python3
import os
import sys
import tempfile


def main():
    dir_path = os.path.dirname(os.path.realpath(__file__))
    files = [
        "styles.css",
        "core.js",
        "shell.js",
        "overview.js",
        "units.js",
        "unit-detail.js",
        "compare.js",
        "views.js",
    ]

    contents = {}
    for f in files:
        path = os.path.join(dir_path, f)
        try:
            with open(path, "r", encoding="utf-8") as file:
                contents[f] = file.read()
        except OSError as e:
            print(f"Error reading {path}: {e}", file=sys.stderr)
            sys.exit(1)

    html = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>llvm-advisor</title>
  <meta name="description" content="LLVM Advisor — Compilation analysis and optimization insights">
  <meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self';">
  <style>
{contents["styles.css"]}
  </style>
</head>
<body>
  <div id="app"></div>
  <script>
{contents["core.js"]}
  </script>
  <script>
{contents["shell.js"]}
  </script>
  <script>
{contents["overview.js"]}
  </script>
  <script>
{contents["units.js"]}
  </script>
  <script>
{contents["unit-detail.js"]}
  </script>
  <script>
{contents["compare.js"]}
  </script>
  <script>
{contents["views.js"]}
  </script>
  <script>
    (function() {{
      Router.register('/', () => OverviewView.render());
      Router.register('/units', () => UnitsView.render());
      Router.register('/units/:id', params => UnitDetailView.render(params));
      Router.register('/compare', params => CompareView.render(params));
      Router.register('/timeline', () => TimelineView.render());
      Router.register('/insights', () => InsightsView.render());
      Router.register('/settings', () => SettingsView.render());
      Shell.init();
      Keys.init();
      Router.init();
    }})();
  </script>
</body>
</html>"""

    # Validate raw-string delimiter collision
    delimiter = ")LLVMHTML"
    if delimiter in html:
        print(
            f"Error: generated HTML contains raw-string delimiter '{delimiter}'. "
            "Refuse to emit broken C++ literal.",
            file=sys.stderr,
        )
        sys.exit(1)

    # Generate the .inc file for C++
    inc_content = f"""// AUTO-GENERATED — do not edit. Run Assets/bundle.py to regenerate.
// clang-format off
static const char IndexHTML[] = R"LLVMHTML(
{html}
)LLVMHTML";
// clang-format on
"""

    def atomic_write(path, data):
        output_dir = os.path.dirname(os.path.abspath(path))
        os.makedirs(output_dir, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=output_dir, suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(data)
            os.replace(tmp, path)
        except OSError as e:
            print(f"Error writing {path}: {e}", file=sys.stderr)
            try:
                os.unlink(tmp)
            except OSError:
                pass
            sys.exit(1)

    inc_path = (
        sys.argv[1] if len(sys.argv) > 1 else os.path.join(dir_path, "index_html.inc")
    )
    html_path = sys.argv[2] if len(sys.argv) > 2 else None

    atomic_write(inc_path, inc_content)
    if html_path:
        atomic_write(html_path, html)

    print(f"Generated {inc_path} ({len(inc_content)} bytes)")
    if html_path:
        print(f"Generated {html_path} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
