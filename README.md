Question 1: Symlinks vs. Hard Links
Explain the difference between a symbolic link and a hard link. Why can a symbolic link cause an infinite loop during directory traversal, but a hard link to a directory cannot? (Hint: think about what the operating system allows you to create.)

A hard link is another entry in a directory table that points to a file (have the same inode and data). A symlink is essentially a path to a file or directory, it has its own inode but contains a path to the same data. A symlink can create an infinite loop because it can point to an ancestor directory and live in a sub directory. A hard link cannot cause an infinite loop because the OS doesn't allow a hardlink to a directory for that exact reason. If you were to create a hardlink to a directory, the filesystem would be corrupted. 


Question 2: Cycle Detection
Your cycle detection uses (st_dev, st_ino) pairs to identify directories. Why are both fields necessary? Give a concrete example of a scenario where checking only st_ino would incorrectly detect a cycle (or miss one).

Both fields are necessary because when you create a symlink, the st_dev of the directory and the symlink that was just created could have different st_devs. If you were to only check the inode number, there could be an issue when looking at files on different filesystems because inodes are only unique within a specific filesystem. This means that if a symlink were to cross a filesystem boundary and points to a directory on a different device that shares an inode number with one that is already visited, only checking the inode would incorrectly detect a cycle. Including the specific device makes sure that the inode and device pair are unique across all mounted filesystems.


Question 3: Filesystem Boundaries
When you run bfind / -xdev -name "*.conf", the traversal skips directories like /proc and /sys. Explain how your implementation detects that these directories are on a different filesystem. What field in struct stat changes when you cross a mount point, and why?

It skips these directories because they are on a different file system than root. My implementation detects that these are on a different filesystem by checking the current directory's dev with it's starting paths (input directory) dev. The device field changes when you cross a mount point because each filesystem gets assigned a unique device id when it is mounted. /proc and /sys are each mounted separately, so their st_dev field differs from /.


Question 4: The VFS Layer
Linux presents a single unified directory tree (starting at /) even though it is composed of many different filesystems. Briefly explain the role of the VFS (Virtual File System) layer in making this possible. Why does the VFS need to exist — why can’t user programs just talk directly to each filesystem driver?

The virtual filesystem acts as an abstraction layer between the user programs and filesystem drivers. Without the VFS, each program would need to know which filesystem it is talking to. The VFS provides a single interface that works regardless of the filesystem by translating calls (like read, write, open, close) into filesystem-specific operations.

