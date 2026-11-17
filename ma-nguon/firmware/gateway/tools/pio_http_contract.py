# Executed by PlatformIO/SCons with its selected F-drive Python environment.
from pathlib import Path
import runpy
Import("env")
runpy.run_path(str(Path(env.subst("$PROJECT_DIR")) / "tools/http_contract.py"), run_name="__main__")
