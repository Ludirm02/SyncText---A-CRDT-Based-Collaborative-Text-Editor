README — SyncText Collaborative Text Editor
Project Title:

SyncText: A Lock-Free Collaborative Text Editor using POSIX Message Queues and Shared Memory

Author:

Mridul Kumar
M.Tech, Department of Computer Science and Engineering
IIT Kharagpur

1. Compilation Instructions

Follow the steps below to compile the program:

# Step 1: Ensure required libraries are available
sudo apt-get install build-essential

# Step 2: Compile the program
g++ -std=c++17 -O2 own.cpp -o editor -lrt -pthread


Explanation:

-std=c++17 → Enables modern C++ features.

-O2 → Optimizes code for faster runtime.

-lrt → Links the POSIX real-time library (required for message queues).

-pthread → Enables multi-threading (for the listener thread).

After compilation, an executable named editor will be generated.

2. Execution Instructions
To run the system for multiple users:

Each user runs the program in a separate terminal window (or on different machines using shared memory):

Terminal 1: ./editor user_1
Terminal 2: ./editor user_2
Terminal 3: ./editor user_3


Each process will automatically:

Register itself in the shared memory registry.

Create its own message queue (/queue_user_<id>).

Open or create its text file (user_<id>_doc.txt).

Continuously monitor file changes and synchronize updates.

File names generated:

user_1_doc.txt

user_2_doc.txt

user_3_doc.txt

Editing Flow:

Each user edits their respective document (using any text editor or nano/vim).

On saving changes, updates are automatically broadcast to all other users.

After every 5 updates (global threshold), a merge is triggered across all users.

The output on each terminal shows synchronized document content and recent activity logs.

3. Dependencies

The following libraries and headers are required:

Dependency	Purpose
pthread	Multi-threading support for listener thread
rt	Real-time extensions used for POSIX message queues
sys/mman.h	Shared memory creation (shm_open, mmap)
mqueue.h	POSIX message queue APIs (mq_open, mq_send, mq_receive)
unistd.h, fcntl.h, sys/stat.h	File I/O, polling, and stat monitoring
atomic, chrono, thread	Lock-free synchronization and timing

All are available by default in Linux distributions.

4. Platform Information
Component	Specification
Operating System	Ubuntu 20.04 LTS (or newer)
Compiler	g++ 9.4.0 or later
Kernel Requirement	Supports POSIX shared memory and message queues
Hardware	Multi-core system (tested on x86_64)

To verify message queue support:

mount -t mqueue none /dev/mqueue

5. How to Test the System
Step 1: Start Multiple Instances

Open 3 different terminal windows:

./editor user_1
./editor user_2
./editor user_3


Each instance will display:

[MQ] created queue: /queue_user_1 (msgsize=1024,maxmsg=10)
Registered as user_1 in slot 0
Monitoring for changes...

Step 2: Edit and Save

Open one of the user files in any editor:

nano user_1_doc.txt


Modify a line and save (Ctrl + O, Enter, Ctrl + X).

You should see messages like:

[LOCAL] Detected change at line 2
[Broadcast] Sent 1 updates to other users.


And in the other terminals:

[Listener] Buffered 1 remote updates.
[Merge] Applied line 2 from user_1 (ts: 1731347500567100)

Step 3: Trigger a Merge

After 5 total updates (across all users), the editor will print:

[Merge] Triggering merge with 2 local and 3 remote updates.
[Merge] Applied line 1 from user_2 (ts: 1731347500799930)


All document copies (user_1_doc.txt, user_2_doc.txt, user_3_doc.txt) will now have identical content.

Step 4: Verify Consistency

Run:

diff user_1_doc.txt user_2_doc.txt
diff user_2_doc.txt user_3_doc.txt


Both should return no output, confirming perfect synchronization.

6. System Behavior Overview

The editor polls the file system every 200ms for local edits.

Broadcasts new updates to all other active queues.

Maintains lock-free consistency using atomic operations.

Performs deterministic merging after 5 updates using Last-Writer-Wins logic.

Log messages include:

[Broadcast] Sent N updates to other users.
[Listener] Buffered N remote updates.
[Merge] Triggering merge...
[Merge] Applied line X from user_Y.


These messages verify inter-process communication and synchronization.

7. Cleanup and Reset

If you forcibly stop all editors (e.g., Ctrl+C) and want a clean start:

sudo rm /dev/mqueue/queue_user_* 2>/dev/null
sudo shm_unlink /synctext_registry_v1


Then rerun ./editor user_1 to reinitialize shared memory.

8. Known Limitations

Edits are line-based, not character-based.

Only one merge is triggered after every 5 updates.

Works reliably on Linux/Unix; not supported on Windows natively.

Requires /dev/mqueue to be mounted for message queues to function.

9. Expected Output Summary

Example synchronized terminal output:

[MQ] created queue: /queue_user_1 (msgsize=1024,maxmsg=10)
Registered as user_1 in slot 0
Document: user_1_doc.txt
----------------------------------------
Line 0: Hello World
Line 1: This is a collaborative editor
Line 2: Welcome
----------------------------------------
Active users: user_1, user_2, user_3
Monitoring for changes...
[Broadcast] Sent 1 updates to other users.
[Listener] Buffered 1 remote updates.
[Merge] Applied line 2 from user_2 (ts: 1731347500567100)