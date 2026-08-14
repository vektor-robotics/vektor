# Contributing to VEKTOR

VEKTOR targets Ubuntu 24.04 and ROS 2 Jazzy. Before opening a pull request:

```bash
source /opt/ros/jazzy/setup.bash
rosdep install --from-paths . --ignore-src -r -y
colcon build --symlink-install --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo
colcon test --packages-select vektor
colcon test-result --verbose
```

Format changed C++ files with `clang-format`. Keep changes focused, add tests for
new behavior, and update the README or changelog when user-facing behavior changes.

Use a short-lived branch and open a pull request against `main`. Explain the
problem, the chosen behavior, and how it was verified. All CI checks must pass
before merge.
