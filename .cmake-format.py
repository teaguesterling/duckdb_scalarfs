# cmake-format configuration for scalarfs. Same settings as plinking_duck's.
#
# A .py config (not .yaml): cmakelang loads a YAML config by importing PyYAML, and
# DuckDB's format venv does not install it, so a .cmake-format.yaml crashes the
# Format Check job rather than failing it.
#
# enable_markup is OFF so cmake-format does not reflow comment bodies: the comments
# in CMakeLists.txt carry measured reasons (the MSVC C++17 note) and must stay as
# written. tab_size is 4 to match this file's existing indentation.
enable_markup = False
line_width = 100
tab_size = 4
