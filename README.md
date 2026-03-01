Question 1: Symlinks vs. Hard Links
Explain the difference between a symbolic link and a hard link. Why can a symbolic link cause an infinite loop during directory traversal, but a hard link to a directory cannot? (Hint: think about what the operating system allows you to create.)




Question 2: Cycle Detection
Your cycle detection uses (st_dev, st_ino) pairs to identify directories. Why are both fields necessary? Give a concrete example of a scenario where checking only st_ino would incorrectly detect a cycle (or miss one).




Question 3: Filesystem Boundaries
When you run bfind / -xdev -name "*.conf", the traversal skips directories like /proc and /sys. Explain how your implementation detects that these directories are on a different filesystem. What field in struct stat changes when you cross a mount point, and why?





Question 4: The VFS Layer
Linux presents a single unified directory tree (starting at /) even though it is composed of many different filesystems. Briefly explain the role of the VFS (Virtual File System) layer in making this possible. Why does the VFS need to exist — why can’t user programs just talk directly to each filesystem driver?



