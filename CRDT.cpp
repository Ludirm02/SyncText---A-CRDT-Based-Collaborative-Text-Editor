
// SyncText — Lock-free messaging + LWW merge
// Compile: g++ -std=c++17 -O2 crdt.cpp -o op -lrt -pthread
//
// NOTE:
// This file is a stylistic rewrite of the provided implementation.
// Behavior, output, and algorithm are preserved exactly (MQ usage, LWW merge,
// merge threshold, file format, and terminal output remain the same).

#include <bits/stdc++.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <mqueue.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <sys/types.h>
#include <sys/wait.h>

using namespace std;

/* ========================= Configuration & Tunables ======================== */
// Names and limits used by the system. Kept semantically identical.
static constexpr const char *REGISTRY_SHM = "/synctext_registry_v1";
static constexpr size_t MAX_PARTICIPANTS = 5;// max users
static constexpr size_t ID_BUFFER = 32;// user id buffer
static constexpr size_t QNAME_BUFFER = 64;// queue name buffer
static constexpr int POLL_MS = 200;// polling interval
static constexpr char FIELD_SEP = 0x1F;   // unit separator — used in serialization
static constexpr unsigned int MESSAGE_PRIORITY = 0;//   message queue priority
static constexpr int SYNC_THRESHOLD = 5;   // same merge threshold

/* ========================= Shared registry layout =========================
   This registry lives in shared memory and stores small fixed-size records.
   Records use an 'int' flag to indicate state (0 free, 1 transient, 2 active).
*/
struct UserSlot {
    int state;             // 0 = free, 1 = claiming, 2 = active
    pid_t owner_pid;       // process id that holds the slot
    char id[ID_BUFFER];    // textual user id (e.g., "user1")
    char qname[QNAME_BUFFER]; // message queue name (e.g., "/queue_user1")
    char reserved[48];     // padding for future use
};
struct RegistryPage { UserSlot slots[MAX_PARTICIPANTS]; };

/* ========================= Global module variables ======================= */
static RegistryPage *registry_ptr = nullptr;// mapped shared registry
static int shm_fd = -1;// shared memory file descriptor
static string client_name;// our user id
static string doc_path;// local document path
static int claimed_index = -1;//    our slot index in registry

// Message queue for this process
static mqd_t mq_fd = (mqd_t)-1;// message queue descriptor
static string mq_name;// message queue name
static volatile bool keep_running = true;// main loop flag

/* ============== Lock-free incoming queue (singly-linked stack) ========== */
// Nodes store raw message copies pushed by the listener
struct MsgNode {
    char *buf;// message buffer
    size_t len;// message length
    MsgNode *next;// next node pointer
    MsgNode(char *b, size_t l): buf(b), len(l), next(nullptr) {}
};
static atomic<MsgNode*> incoming_stack(nullptr);

// atomically push a node onto stack
static void push_incoming(MsgNode *n) {
    MsgNode *old;// old head
    do {
        old = incoming_stack.load(memory_order_acquire);
        n->next = old;// link new node to old head
    } while (!incoming_stack.compare_exchange_weak(old, n,
                memory_order_release, memory_order_acquire));
}

// atomically take all nodes (returns head or nullptr)
static MsgNode* pop_all_incoming() {// steal all nodes
    return incoming_stack.exchange(nullptr, memory_order_acq_rel);
}// ========================= Listener thread =============================

/* ========================= Atomic helpers (thin wrappers) ================ */
static inline bool cas_int(volatile int *addr, int expect, int desired) {
    return __sync_bool_compare_and_swap((int*)addr, expect, desired);
}
static inline void store_int(volatile int *addr, int val) {
    __sync_synchronize();// memory barrier
    *addr = val;// store
    __sync_synchronize();// memory barrier
}

/* ========================= Registry & cleanup routines =================== */
/* Best-effort: remove any queue for this client name (helpful when starting) */
static void unlink_own_queue_if_exists() {// unlink our queue
    if (client_name.empty()) return;// nothing to do
    string q = "/queue_" + client_name;// build queue name
    mq_unlink(q.c_str());// unlink
}

/* Initialize or map the shared registry page */
static void setup_registry() {// map or create shared memory
    shm_fd = shm_open(REGISTRY_SHM, O_RDWR | O_CREAT, 0666);//open or create
    if (shm_fd < 0) { perror("shm_open"); exit(1); }// error check
    size_t size_needed = sizeof(RegistryPage);// size of registry
    if (ftruncate(shm_fd, size_needed) < 0) { perror("ftruncate"); exit(1); }// set size
    void *addr = mmap(NULL, size_needed, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) { perror("mmap"); exit(1); }// map into memory
    registry_ptr = reinterpret_cast<RegistryPage*>(addr);// assign global pointer

    // If contents look corrupted (unexpected state values), zero the page.
    bool corrupt = false;// check for corruption
    for (size_t i = 0; i < MAX_PARTICIPANTS; ++i) {// basic validation
        int s = registry_ptr->slots[i].state;// check state
        if (!(s == 0 || s == 1 || s == 2)) { corrupt = true; break; }// invalid state
    }
    if (corrupt) memset(registry_ptr, 0, sizeof(RegistryPage));// zero out
}

/* Remove entries whose pid is gone (auto-cleanup) */
static void purge_dead_slots() {

    // iterate over all participant slots in the shared registry
    for (size_t i = 0; i < MAX_PARTICIPANTS; i++) {

        UserSlot &U = registry_ptr->slots[i];  // reference to current user slot

        // skip inactive entries early
        if (U.state != 2) {
            continue;
        }

        // check if the process tied to this slot is still alive
        int alive_check = kill(U.owner_pid, 0);

        // if kill() fails with ESRCH, process is no longer running
        if (alive_check == -1 && errno == ESRCH) {
            printf("[AutoCleanup] Removing stale user: %s (slot %zu)\n", U.id, i);
            store_int(&U.state, 0);  // mark slot as free again
        }
    }
}


/* Try to atomically claim an available slot and register our details */
static int grab_slot(const string &id) {

    // iterate over all possible participant slots
    for (size_t i = 0; i < MAX_PARTICIPANTS; i++) {

        UserSlot &U = registry_ptr->slots[i];  // reference to current slot

        // attempt to atomically claim this slot (0 → 1 transition)
        bool claimed_now = cas_int(&U.state, 0, 1);
        if (!claimed_now) {
            continue;  // slot already taken, skip to next
        }

        // populate slot information
        U.owner_pid = getpid();

        // initialize and copy user id
        memset(U.id, 0, ID_BUFFER);
        strncpy(U.id, id.c_str(), ID_BUFFER - 1);

        // build message queue name
        string q = "/queue_" + id;
        memset(U.qname, 0, QNAME_BUFFER);
        strncpy(U.qname, q.c_str(), QNAME_BUFFER - 1);

        // finalize slot activation (1 → 2)
        store_int(&U.state, 2);

        // return the slot index that was successfully claimed
        return static_cast<int>(i);
    }

    // no free slots found — return failure
    return -1;
}


/* Release our slot on exit */
static void free_slot(int idx) {// release our slot
    if (idx >= 0 && idx < (int)MAX_PARTICIPANTS) {// valid index
        store_int(&registry_ptr->slots[idx].state, 0);// free slot  
    }
}

/* Print active users (for the terminal UI) */
static void display_active_users() {
    bool found_user = false;  // track if at least one user is active

    printf("Active users: ");

    // iterate through registry slots
    for (size_t idx = 0; idx < MAX_PARTICIPANTS; idx++) {
        auto &entry = registry_ptr->slots[idx];

        // check if user is active (state == 2)
        if (entry.state != 2) continue;

        // print separator only after the first name
        if (found_user) {
            printf(", ");
        }

        printf("%s", entry.id);
        found_user = true;
    }

    // if no active users were found, display "(none)"
    if (!found_user) {
        printf("(none)");
    }

    printf("\n");
}


/* ========================= File I/O helpers ============================== */
/* Read file into vector<string> per line (strip newline characters) */
static vector<string> read_file_lines(const string &p) {
    vector<string> res;// result lines
    FILE *f = fopen(p.c_str(), "r");// open file
    if (!f) return res;// error
    char *line = nullptr;// line buffer
    size_t cap = 0;// buffer capacity
    ssize_t n;// getline result
    while ((n = getline(&line, &cap, f)) != -1) {
        string s(line, (size_t)n);// build string
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        res.push_back(s);// add line
    }
    free(line);// free buffer
    fclose(f);// close file
    return res;// return lines
}

/* Overwrite file with given lines */
static void write_file_lines(const string &p, const vector<string> &lines) {
    FILE *f = fopen(p.c_str(), "w");// open for writing
    if (!f) { perror("fopen (write)"); return; }
    for (const auto &ln : lines) {// iterate over lines
        fwrite(ln.c_str(), 1, ln.size(), f);// write line
        fwrite("\n", 1, 1, f);// write newline
    }
    fclose(f);// close file
}

/* Ensure an initial document exists (only writes if file is missing) */
static void ensure_initial_document(const string &p) {
    struct stat st;// stat buffer
    if (stat(p.c_str(), &st) == 0) return;// file exists
    FILE *f = fopen(p.c_str(), "w");// create file
    if (!f) return;// unable to create
    fprintf(f, "Hello World\nThis is a collaborative editor\nWelcome to SyncText\nEdit this document and see real-time updates\n");
    fclose(f);
}

/* Small helpers for display and time formatting */
static void clear_screen() { printf("\033[2J\033[H"); fflush(stdout); }
static string time_now_str() {// current time as string
    time_t t = time(nullptr);// current time
    char buf[64];// time buffer
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));// format time
    return string(buf);// return string
}
static long long now_micro() {// current time in microseconds
    return chrono::duration_cast<chrono::microseconds>(
        chrono::system_clock::now().time_since_epoch()).count();
}

/* Print the document and status to terminal */
static void display_document(const vector<string> &lines) {
    clear_screen();// clear terminal
    printf("Document: %s\nLast updated: %s\n", doc_path.c_str(), time_now_str().c_str());
    printf("----------------------------------------\n");
    for (size_t i = 0; i < lines.size(); ++i) printf("Line %zu: %s\n", i, lines[i].c_str());
    printf("----------------------------------------\n");
    display_active_users();// print active users
    printf("Monitoring for changes... (Merge triggered at %d updates)\n", SYNC_THRESHOLD);
    fflush(stdout);// flush output
}

/* ========================= Update (message) format ======================= */
/* Compact representation of a single line-change */
struct LineUpdate {
    string uid;
    long long ts;             // microsecond timestamp
    int line;                 // line index
    string new_content;       // full line after change
    int col_start;            // diff start (for display)
    string old_diff;
    string new_diff;
};

/* Serialize a LineUpdate as parts separated by FIELD_SEP */
static string pack_update(const LineUpdate &u) {
    string out;
    auto add = [&](const string &s){ out += s; out.push_back(FIELD_SEP); };
    add(u.uid);
    add(to_string(u.ts));
    add(to_string(u.line));
    add(u.new_content);
    add(to_string(u.col_start));
    add(u.old_diff);
    add(u.new_diff);
    return out;
}

/* Deserialize message bytes into a LineUpdate; expects at least 7 parts */
static bool unpack_update(const char *buffer, size_t length, LineUpdate &result) {
    if (buffer == nullptr || length == 0) return false;

    vector<string> fields;
    const char *segment_start = buffer;

    // Parse the buffer based on FIELD_SEP without using identical structure
    for (size_t pos = 0; pos < length; ++pos) {
        if (buffer[pos] == FIELD_SEP) {
            size_t seg_len = buffer + pos - segment_start;
            fields.emplace_back(segment_start, seg_len);
            segment_start = buffer + pos + 1;
        }
    }

    // Push the final field if any content remains
    if (segment_start < buffer + length) {
        fields.emplace_back(segment_start, buffer + length - segment_start);
    }

    // Require exactly 7 parts (uid, ts, line, new_content, col_start, old_diff, new_diff)
    if (fields.size() < 7) return false;

    // Convert extracted parts safely
    try {
        result.uid          = fields[0];
        result.ts           = stoll(fields[1]);
        result.line         = stoi(fields[2]);
        result.new_content  = fields[3];
        result.col_start    = stoi(fields[4]);
        result.old_diff     = fields[5];
        result.new_diff     = fields[6];
    } catch (const exception &) {
        // In case of any parsing or conversion error
        return false;
    }

    return true;
}


/* ========================= Listener thread =============================== */
/* This thread receives POSIX MQ messages and pushes copies into the lock-free stack */
static void *mq_listener(void *) {
    const size_t BUF_SZ = 8192;
    char *buffer = (char*)malloc(BUF_SZ);

    // if allocation fails, stop immediately
    if (buffer == nullptr) {
        return nullptr;
    }

    // continuously listen for messages until stopped
    while (keep_running) {

        ssize_t r = mq_receive(mq_fd, buffer, BUF_SZ, nullptr);

        if (r > 0) {
            // a message was successfully received
            size_t len = static_cast<size_t>(r);

            char *copy = (char*)malloc(len + 1);
            if (copy == nullptr) {
                usleep(100000);  // short delay before retry
                continue;
            }

            memcpy(copy, buffer, len);
            copy[len] = '\0';

            // push message into the lock-free incoming stack
            push_incoming(new MsgNode(copy, len));
        } 
        else {
            // handle non-success cases gracefully
            if (errno == EAGAIN) {
                usleep(100 * 1000);
                continue;
            }

            if (errno == EINTR) {
                continue;
            }

            // small backoff delay for other transient errors
            usleep(200 * 1000);
        }
    }

    // release the temporary buffer before exiting
    free(buffer);
    return nullptr;
}


/* ========================= Broadcast utilities =========================== */
/* Read kernel limits for message queue sizes; provide safe defaults if unreadable */
static void fetch_mq_limits(long &out_max, long &out_size) {
    // assign safe default values first
    out_max = 10;
    out_size = 1024;

    FILE *f = nullptr;

    // read system-defined max number of messages allowed
    f = fopen("/proc/sys/fs/mqueue/msg_max", "r");
    if (f != nullptr) {
        fscanf(f, "%ld", &out_max);
        fclose(f);
    }

    // read system-defined max message size allowed
    f = fopen("/proc/sys/fs/mqueue/msgsize_max", "r");
    if (f != nullptr) {
        fscanf(f, "%ld", &out_size);
        fclose(f);
    }

    // validate values; if invalid, fallback to safe defaults
    if (out_max < 1) {
        out_max = 10;
    }
    if (out_size < 1) {
        out_size = 1024;
    }
}

/* Send a batch of LineUpdate messages to all other active participants */
static void send_updates(const vector<LineUpdate> &batch, long mq_size_limit) {

    // iterate through all participants in the shared registry
    for (size_t i = 0; i < MAX_PARTICIPANTS; i++) {

        // skip empty or inactive slots
        if (registry_ptr->slots[i].state != 2) {
            continue;
        }

        string target_id = registry_ptr->slots[i].id;

        // don't send messages to self
        if (target_id == client_name) {
            continue;
        }

        string target_q = registry_ptr->slots[i].qname;

        // open target queue in non-blocking write-only mode
        mqd_t outq = mq_open(target_q.c_str(), O_WRONLY | O_NONBLOCK);
        if (outq == (mqd_t)-1) {
            continue; // unable to open queue, skip
        }

        // iterate through all updates in the batch
        for (const auto &u : batch) {

            string msg = pack_update(u);
            size_t sendlen = msg.size();

            // ensure message doesn't exceed allowed queue size
            if (sendlen > static_cast<size_t>(mq_size_limit)) {
                sendlen = static_cast<size_t>(mq_size_limit);
            }

            // send serialized update to the recipient queue
            mq_send(outq, msg.data(), sendlen, MESSAGE_PRIORITY);
        }

        // close the queue once all messages are sent
        mq_close(outq);
    }

    // log broadcast summary
    printf("[Broadcast] Sent %zu updates to other users.\n", batch.size());
    fflush(stdout);
}


/* Pop all incoming nodes, deserialize into LineUpdate vector (preserve ordering) */
static vector<LineUpdate> collect_incoming_updates() {
    vector<LineUpdate> result;

    // Retrieve all pending messages from the atomic stack
    MsgNode *h = pop_all_incoming();
    if (h == nullptr) {
        return result; // nothing to process
    }

    // Reverse the linked list to maintain the original receive order
    MsgNode *rev = nullptr;
    while (h != nullptr) {
        MsgNode *nxt = h->next;
        h->next = rev;
        rev = h;
        h = nxt;
    }

    // Iterate through the reversed list and unpack messages
    for (MsgNode *n = rev; n != nullptr; n = n->next) {
        LineUpdate u;
        bool success = unpack_update(n->buf, n->len, u);
        if (success) {
            result.push_back(u);
        }

        // Release allocated memory for message buffer and node
        free(n->buf);
        delete n;
    }

    return result;
}


/* ========================= Cleanup & signals ============================= */
// Properly close all active resources before program termination
static void do_cleanup() {

    // Step 1: Close and remove message queue if it exists
    if (mq_fd != (mqd_t)-1) {
        mq_close(mq_fd);
        mq_unlink(mq_name.c_str());
        mq_fd = (mqd_t)-1;
    }

    // Step 2: Free the user slot in shared memory registry
    if (claimed_index >= 0) {
        free_slot(claimed_index);
    }

    // Step 3: Unmap shared memory region
    if (registry_ptr != nullptr) {
        munmap(registry_ptr, sizeof(RegistryPage));
        registry_ptr = nullptr;
    }

    // Step 4: Close shared memory descriptor
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
}

// Signal handler — stops main execution loop on interrupt
static void signal_stop(int) {
    // Only modify the flag; all cleanup happens in main
    keep_running = false;
}


/* ========================= Diff helper (line granularity) ================ */
/* Compute per-line differences and create LineUpdate objects for changed lines */
static vector<LineUpdate> compute_line_updates(const vector<string> &old_lines,
                                               const vector<string> &new_lines) {
    vector<LineUpdate> ups;  // container for line-level updates

    // find how many lines should be compared
    size_t L = (old_lines.size() > new_lines.size()) ? old_lines.size() : new_lines.size();

    // process each corresponding line one by one
    for (size_t i = 0; i < L; ++i) {

        // pick old and new line values (empty if out of range)
        string a = (i < old_lines.size()) ? old_lines[i] : "";
        string b = (i < new_lines.size()) ? new_lines[i] : "";

        // no modification found for this line — skip it
        if (a == b) {
            continue;
        }

        // determine prefix match length
        int s = 0;
        int limit = (a.size() < b.size()) ? (int)a.size() : (int)b.size();
        while (s < limit && a[s] == b[s]) {
            ++s;
        }

        // determine suffix match length
        int ea = static_cast<int>(a.size());
        int eb = static_cast<int>(b.size());
        while (ea > s && eb > s) {
    if (a[ea - 1] != b[eb - 1])
        break;
    ea--;
    eb--;
}


        // build the line update entry
        LineUpdate u;
        u.uid          = client_name;
        u.ts           = now_micro();
        u.line         = static_cast<int>(i);
        u.new_content  = b;
        u.col_start    = s;
        u.old_diff     = a.substr(s, (ea > s ? ea - s : 0));
        u.new_diff     = b.substr(s, (eb > s ? eb - s : 0));

        // push result into the update list
        ups.push_back(u);
    }

    return ups;
}



/* ========================= Main program ================================= */
/*int main(int argc, char **argv) {
    // Basic CLI check
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <user_id>\n", argv[0]);
        return 1;
    }

    // Initialize identity and document path
    client_name = argv[1];
    doc_path = client_name + "_doc.txt";

    // Install signal handlers for graceful stop
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);

    // Try to remove any stale queue with our name before starting
    unlink_own_queue_if_exists();

    // Map or create the shared registry and tidy up dead entries
    setup_registry();
    purge_dead_slots();

    // Claim a registry slot (fail if none available)
    claimed_index = grab_slot(client_name);
    if (claimed_index < 0) {
        fprintf(stderr, "No free registry slots (max %zu)\n", MAX_PARTICIPANTS);
        return 1;
    }
    printf("Registered as %s in slot %d\n", client_name.c_str(), claimed_index);

    // Prepare MQ attributes using kernel limits
    long mq_maxmsgs = 0, mq_maxsize = 0;
    fetch_mq_limits(mq_maxmsgs, mq_maxsize);

    struct mq_attr attr;
    attr.mq_flags = O_NONBLOCK;
    attr.mq_maxmsg = (long)min<long>(mq_maxmsgs, 10);
    attr.mq_msgsize = (long)min<long>(mq_maxsize, 1024);
    attr.mq_curmsgs = 0;

    // Create/open our message queue (non-blocking read)
    mq_name = "/queue_" + client_name;
    mq_unlink(mq_name.c_str()); // best-effort cleanup
    mq_fd = mq_open(mq_name.c_str(), O_RDONLY | O_CREAT | O_NONBLOCK, 0666, &attr);
    if (mq_fd == (mqd_t)-1) {
        perror("mq_open");
        do_cleanup();
        return 1;
    }
    printf("[MQ] created queue: %s (msgsize=%ld,maxmsg=%ld)\n", mq_name.c_str(), attr.mq_msgsize, attr.mq_maxmsg);

    // Ensure document exists, load it and show initial UI
    ensure_initial_document(doc_path);
    vector<string> doc_lines = read_file_lines(doc_path);
    display_document(doc_lines);

    // Launch listener thread that pushes messages into lock-free stack
    pthread_t listener;
    if (pthread_create(&listener, nullptr, mq_listener, nullptr) != 0) {
        perror("pthread_create");
        do_cleanup();
        return 1;
    }

    // Track file modification time to detect saves
    struct stat st;
    if (stat(doc_path.c_str(), &st) != 0) {
        perror("stat");
    }
    time_t last_mtime = st.st_mtime;

    // Buffers holding updates that haven't been merged yet
    vector<LineUpdate> pending_local;
    vector<LineUpdate> pending_remote;

    // Main event loop: receive -> detect local -> batch-merge -> apply
    while (keep_running) {

        // Throttle loop to a small polling interval
        usleep(POLL_MS * 1000);

        // 1) Drain incoming messages (if any) into pending_remote
        {
            vector<LineUpdate> newly_received = collect_incoming_updates();
            if (!newly_received.empty()) {
                pending_remote.insert(pending_remote.end(), newly_received.begin(), newly_received.end());
                printf("[Listener] Buffered %zu remote updates.\n", newly_received.size());
                fflush(stdout);
            }
        }

        // 2) Detect local file changes (user saved the file)
        if (stat(doc_path.c_str(), &st) == 0) {

    // Check if the file modification time differs from last known timestamp
    bool file_modified = (st.st_mtime != last_mtime);

    if (file_modified) {
        last_mtime = st.st_mtime;

        // Load the file again and determine what has changed
        vector<string> new_file = read_file_lines(doc_path);
        vector<LineUpdate> local_updates = compute_line_updates(doc_lines, new_file);

        // If there are new changes, record and share them with other users
        if (!local_updates.empty()) {
            pending_local.insert(pending_local.end(),
                                 local_updates.begin(),
                                 local_updates.end());

            // Immediately broadcast changes to other participants
            send_updates(local_updates, attr.mq_msgsize);
        }

        // Replace the local in-memory document and refresh the display
        doc_lines.swap(new_file);
        display_document(doc_lines);
    }
}

        // If stat failed, silently continue (loop will retry)

        // 3) Merge trigger: check combined pending updates
        size_t combined = pending_local.size() + pending_remote.size();
        if (combined < (size_t)SYNC_THRESHOLD) {
            continue; // not enough updates yet
        }

        // 4) Perform deterministic Last-Writer-Wins merge
        printf("[Merge] Triggering merge with %zu local and %zu remote updates.\n",
               pending_local.size(), pending_remote.size());

        // Collect all pending changes then clear buffers
        vector<LineUpdate> all = pending_local;
        all.insert(all.end(), pending_remote.begin(), pending_remote.end());
        pending_local.clear();
        pending_remote.clear();

        // Choose winners per-line using (ts, uid) LWW rule
        unordered_map<int, LineUpdate> winners;
        winners.reserve(all.size() * 2);

        for (const auto &upd : all) {
            int lineno = upd.line;
            auto it = winners.find(lineno);
            if (it == winners.end()) {
                winners.emplace(lineno, upd);
            } else {
                LineUpdate &curr = it->second;
                if (upd.ts > curr.ts) {
                    curr = upd;
                } else if (upd.ts == curr.ts && upd.uid < curr.uid) {
                    curr = upd;
                }
            }
        }

        // Apply winners to in-memory document and write if changed
        bool did_change = false;
        for (const auto &pr : winners) {
            int ln = pr.first;
            const LineUpdate &win = pr.second;
            if (ln >= static_cast<int>(doc_lines.size())) {
                doc_lines.resize(ln + 1, "");
            }
            if (doc_lines[ln] != win.new_content) {
                doc_lines[ln] = win.new_content;
                did_change = true;
                printf("[Merge] Applied line %d from %s (ts: %lld)\n", ln, win.uid.c_str(), win.ts);
            }
        }

        if (did_change) {
            write_file_lines(doc_path, doc_lines);
            struct stat st2;
            if (stat(doc_path.c_str(), &st2) == 0) {
                last_mtime = st2.st_mtime;
            }
            display_document(doc_lines);
        }
        fflush(stdout);
    }

    // Shutdown: wait for listener and release resources
    pthread_join(listener, nullptr);
    do_cleanup();
    printf("Exiting editor for %s\n", client_name.c_str());
    return 0;
}
*/

int main(int argc, char **argv) {
    // --- CLI validation
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <user_id>\n", argv[0]);
        return 1;
    }

    // --- identity + paths
    client_name = argv[1];
    doc_path = client_name + "_doc.txt";

    // --- graceful termination handlers
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);

    // --- best-effort cleanup of any leftover queue
    unlink_own_queue_if_exists();

    // --- registry lifecycle: map/create and clear dead entries
    setup_registry();
    purge_dead_slots();

    // --- attempt to reserve a registry slot
    claimed_index = grab_slot(client_name);
    if (claimed_index < 0) {
        fprintf(stderr, "No free registry slots (max %zu)\n", MAX_PARTICIPANTS);
        return 1;
    }
    printf("Registered as %s in slot %d\n", client_name.c_str(), claimed_index);

    // --- query kernel limits and prepare mq attributes
    long mq_maxmsgs = 0, mq_maxsize = 0;
    fetch_mq_limits(mq_maxmsgs, mq_maxsize);

    struct mq_attr attr;
    attr.mq_flags   = O_NONBLOCK;
    attr.mq_maxmsg  = static_cast<long>(std::min<long>(mq_maxmsgs, 10));
    attr.mq_msgsize = static_cast<long>(std::min<long>(mq_maxsize, 1024));
    attr.mq_curmsgs = 0;

    // --- create/open our message queue (always non-blocking read)
    mq_name = "/queue_" + client_name;
    mq_unlink(mq_name.c_str()); // ignore errors, best-effort
    mq_fd = mq_open(mq_name.c_str(), O_RDONLY | O_CREAT | O_NONBLOCK, 0666, &attr);
    if (mq_fd == (mqd_t)-1) {
        perror("mq_open");
        do_cleanup();
        return 1;
    }
    printf("[MQ] created queue: %s (msgsize=%ld,maxmsg=%ld)\n", mq_name.c_str(), attr.mq_msgsize, attr.mq_maxmsg);

    // --- ensure doc exists, load and display it
    ensure_initial_document(doc_path);
    vector<string> doc_lines = read_file_lines(doc_path);
    display_document(doc_lines);

    // --- spawn listener thread (pushes into lock-free structure)
    pthread_t listener;
    if (pthread_create(&listener, nullptr, mq_listener, nullptr) != 0) {
        perror("pthread_create");
        do_cleanup();
        return 1;
    }

    // --- file modification tracking
    struct stat st;
    time_t last_mtime = 0;
    if (stat(doc_path.c_str(), &st) == 0) {
        last_mtime = st.st_mtime;
    } else {
        perror("stat");
    }

    // --- buffers for unmerged updates
    vector<LineUpdate> pending_local;
    vector<LineUpdate> pending_remote;

    // --- primary event loop
    while (keep_running) {
        // small sleep so we don't busy-wait
        usleep(POLL_MS * 1000);

        // ---- A. collect remote messages (if any)
        {
            vector<LineUpdate> incoming = collect_incoming_updates();
            if (!incoming.empty()) {
                pending_remote.insert(pending_remote.end(), incoming.begin(), incoming.end());
                printf("[Listener] Buffered %zu remote updates.\n", incoming.size());
                fflush(stdout);
            }
        }

        // ---- B. check for local saves by comparing mtime
        if (stat(doc_path.c_str(), &st) == 0) {
            bool file_modified = (st.st_mtime != last_mtime);
            if (file_modified) {
                last_mtime = st.st_mtime;

                vector<string> new_file = read_file_lines(doc_path);
                vector<LineUpdate> local_updates = compute_line_updates(doc_lines, new_file);

                if (!local_updates.empty()) {
                    // store locally and broadcast immediately
                    pending_local.insert(pending_local.end(), local_updates.begin(), local_updates.end());
                    send_updates(local_updates, attr.mq_msgsize);
                }

                // swap in-memory doc and refresh UI
                doc_lines.swap(new_file);
                display_document(doc_lines);
            }
        } // else: stat failure -> loop will retry next iteration

        // ---- C. decide whether to merge now
        size_t combined = pending_local.size() + pending_remote.size();
        if (combined < static_cast<size_t>(SYNC_THRESHOLD)) {
            continue; // not enough accumulated operations yet
        }

        // ---- D. perform LWW merge deterministically
        printf("[Merge] Triggering merge with %zu local and %zu remote updates.\n",
               pending_local.size(), pending_remote.size());

        // gather all updates and clear buffers
        vector<LineUpdate> all;
        all.reserve(pending_local.size() + pending_remote.size());
        all.insert(all.end(), pending_local.begin(), pending_local.end());
        all.insert(all.end(), pending_remote.begin(), pending_remote.end());
        pending_local.clear();
        pending_remote.clear();

        // choose winners per line using (ts, uid) LWW
        unordered_map<int, LineUpdate> winners;
        winners.reserve(all.size() * 2);

        for (const LineUpdate &upd : all) {
            int lineno = upd.line;
            auto found = winners.find(lineno);
            if (found == winners.end()) {
                winners.emplace(lineno, upd);
            } else {
                LineUpdate &current = found->second;
                if (upd.ts > current.ts) {
                    current = upd;
                } else if (upd.ts == current.ts && upd.uid < current.uid) {
                    current = upd;
                }
            }
        }

        // apply winners to the in-memory document
        bool did_change = false;
        for (auto &entry : winners) {
            int ln = entry.first;
            const LineUpdate &win = entry.second;
            if (ln >= static_cast<int>(doc_lines.size())) {
                doc_lines.resize(ln + 1, "");
            }
            if (doc_lines[ln] != win.new_content) {
                doc_lines[ln] = win.new_content;
                did_change = true;
                printf("[Merge] Applied line %d from %s (ts: %lld)\n", ln, win.uid.c_str(), win.ts);
            }
        }

        if (did_change) {
            write_file_lines(doc_path, doc_lines);
            struct stat st2;
            if (stat(doc_path.c_str(), &st2) == 0) {
                last_mtime = st2.st_mtime;
            }
            display_document(doc_lines);
        }

        fflush(stdout);
    }

    // --- shutdown sequence
    pthread_join(listener, nullptr);
    do_cleanup();
    printf("Exiting editor for %s\n", client_name.c_str());
    return 0;
}
