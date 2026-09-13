import sys
from pathlib import Path

# The research package lives under python/ rather than being pip-installed, so the
# tests put it on the path explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
