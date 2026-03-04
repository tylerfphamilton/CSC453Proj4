/*
 * bfind - Breadth-first find
 *
 * A BFS version of the UNIX find utility using POSIX system calls.
 *
 * Usage: ./bfind [-L] [-xdev] [path...] [filters...]
 *
 * Filters:
 *   -name PATTERN   Glob match on filename (fnmatch)
 *   -type TYPE      f (file), d (directory), l (symlink)
 *   -mtime N        Modified within the last N days
 *   -size SPEC      File size: [+|-]N[c|k|M]
 *   -perm MODE      Exact octal permission match
 *
 * Options:
 *   -L              Follow symbolic links (default: no)
 *   -xdev           Do not cross filesystem boundaries
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// #include <fcntl.h>
#include "queue.h"
#include <limits.h>

/* ------------------------------------------------------------------ */
/*  Filter definitions                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    FILTER_NAME,
    FILTER_TYPE,
    FILTER_MTIME,
    FILTER_SIZE,
    FILTER_PERM
} filter_kind_t;

typedef enum {
    SIZE_CMP_EXACT,
    SIZE_CMP_GREATER,
    SIZE_CMP_LESS
} size_cmp_t;

typedef struct {
    filter_kind_t kind;
    union {
        char *pattern;       /* -name */
        char type_char;      /* -type: 'f', 'd', or 'l' */
        int mtime_days;      /* -mtime */
        struct {
            off_t size_bytes;
            size_cmp_t size_cmp;
        } size;              /* -size */
        mode_t perm_mode;    /* -perm */
    } filter;
} filter_t;

/* ------------------------------------------------------------------ */
/*  Cycle detection                                                    */
/*                                                                     */
/*  A file's true on-disk identity is its (st_dev, st_ino) pair.       */
/*  You will need this for cycle detection when -L is set.             */
/* ------------------------------------------------------------------ */

typedef struct {
    dev_t dev;
    ino_t ino;
} dev_ino_t;

/* ------------------------------------------------------------------ */
/*  Global configuration                                               */
/* ------------------------------------------------------------------ */

static filter_t *g_filters = NULL;
static int g_nfilters = 0;
static bool g_follow_links = false;
static bool g_xdev = false;
static dev_t g_start_dev = 0;
static time_t g_now;

/* ------------------------------------------------------------------ */
/* Addded Functionality                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    OPTIONS,
    PATHS,
    FILTERS
} parser_phase;

/* ------------------------------------------------------------------ */
/*  Filter matching                                                    */
/* ------------------------------------------------------------------ */

/*
 * TODO 1: Implement this function.
 *
 * Return true if the single filter 'f' matches the file at 'path' with
 * metadata 'sb'. Handle each filter_kind_t in a switch statement.
 *
 * Refer to the assignment document for the specification of each filter.
 * Relevant man pages: fnmatch(3), stat(2).
 */
static bool filter_matches(const filter_t *f, const char *path,
                           const struct stat *sb) {
    /* TODO: Your implementation here */

    bool res = false;

    switch (f->kind){

        case FILTER_NAME:
        {
            // uses fnmatch
            const char* last_path = strrchr(path, '/');

            if (last_path == NULL){
                last_path = (char*) path;
            }

            // need to increment because it is up until the /
            last_path++;
            // printf("the path is: %s\n", last_path);

            if (fnmatch(f->filter.pattern, last_path, 0) == 0){
                res = true;
            }
            break;
        }

        case FILTER_TYPE:
        {           
            if (f->filter.type_char == 'f' && S_ISREG(sb->st_mode)){
                res = true;
            }
            else if (f->filter.type_char == 'd' && S_ISDIR(sb->st_mode)){
                res = true;
            }
            else if (f->filter.type_char == 'l' && S_ISLNK(sb->st_mode)){
                res = true;
            }
            break;
        }

        case FILTER_MTIME:
        {
            // have to use g_now;
            double time_diff = difftime(g_now, sb->st_mtim.tv_sec);
            time_diff /= 86400;
            // printf("the time diff is %f\n", time_diff);

            // need to handle logic correctly now:
            int time_days = (int) time_diff;
            if (time_days <= f->filter.mtime_days){
                res = true;
            }
            break;
        }

        case FILTER_SIZE:
        {
            off_t f_size = f->filter.size.size_bytes;
            size_cmp_t f_cmp = f->filter.size.size_cmp;

            if (f_cmp == SIZE_CMP_GREATER && sb->st_size > f_size){
                res = true;       
            }
            else if (f_cmp == SIZE_CMP_EXACT && sb->st_size == f_size){
                res = true;
            }
            else if (f_cmp == SIZE_CMP_LESS && sb->st_size < f_size){
                res = true;
            }
            break;
        }        
        case FILTER_PERM:
        {
            // printf("the perm is in filter %o and in sb->st_mode: %o\n", f->filter.perm_mode, (sb->st_mode & 0777));

            if (f->filter.perm_mode == (sb->st_mode & 0777)){
                res = true;
            }
            break;
        }
    }
    return res;
}

/* Check if ALL filters match (AND semantics).
 * Returns true if every filter matches, false otherwise. */
static bool matches_all_filters(const char *path, const struct stat *sb) {
    for (int i = 0; i < g_nfilters; i++) {
        if (!filter_matches(&g_filters[i], path, sb)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Usage / help                                                       */
/* ------------------------------------------------------------------ */

static void print_usage(const char *progname) {
    printf("Usage: %s [-L] [-xdev] [path...] [filters...]\n"
           "\n"
           "Breadth-first search for files in a directory hierarchy.\n"
           "\n"
           "Options:\n"
           "  -L              Follow symbolic links\n"
           "  -xdev           Do not cross filesystem boundaries\n"
           "  --help          Display this help message and exit\n"
           "\n"
           "Filters (all filters are ANDed together):\n"
           "  -name PATTERN   Match filename against a glob pattern\n"
           "  -type TYPE      Match file type: f (file), d (dir), l (symlink)\n"
           "  -mtime N        Match files modified within the last N days\n"
           "  -size [+|-]N[c|k|M]\n"
           "                  Match file size (c=bytes, k=KiB, M=MiB)\n"
           "                  Prefix + means greater than, - means less than\n"
           "  -perm MODE      Match exact octal permission bits\n"
           "\n"
           "If no path is given, defaults to the current directory.\n",
           progname);
}

/* ------------------------------------------------------------------ */
/*  Argument parsing                                                   */
/* ------------------------------------------------------------------ */

/*
 * TODO 2: Implement this function.
 *
 * Parse a size specifier string into a byte count. The input is the
 * numeric portion (after any leading +/- is stripped by the caller)
 * with an optional unit suffix: 'c' (bytes), 'k' (KiB), 'M' (MiB).
 * No suffix means bytes.
 *
 * Examples: "100c" -> 100, "4k" -> 4096, "2M" -> 2097152, "512" -> 512
 */
static off_t parse_size(const char *arg) {
    /* TODO: Your implementation here */

    if (arg == NULL){
        // some message
        printf("The arg is NULL in parse_size\n");
        return -1;
    }

    // converting the string to a number
    char* end_ptr;
    off_t res = strtoll(arg, &end_ptr, 10);

    // TODO: check for error here
    if (*end_ptr != '\0' && *end_ptr != 'c' && *end_ptr != 'k' && *end_ptr != 'M'){
        printf("Incorrect usage for parse size argument\n");
        return -1;
    }

    // checking to see if it is a k or M
    if (*end_ptr == 'k'){
        res *= 1024;
    }
    else if (*end_ptr == 'M'){
        res *= 1048576;
    }


    return res;
}

/*
 * TODO 3: Implement this function.
 *
 * Parse command-line arguments into options, paths, and filters.
 * See the usage string and assignment document for the expected format.
 *
 * Set the global variables g_follow_links, g_xdev, g_filters, and
 * g_nfilters as appropriate. Return a malloc'd array of path strings
 * and set *npaths. If no paths are given, default to ".".
 *
 * Handle --help by calling print_usage() and exiting.
 * Exit with an error for unknown options or missing filter arguments.
 */
static char **parse_args(int argc, char *argv[], int *npaths) {

    bool beginning_path = true;
    int paths_start_idx = 0;
    int paths_count = 0;
    bool default_path = false;

    bool beginning_filter = true;
    int filters_start_idx = 0;
    bool size_flag = false;

    parser_phase phase = OPTIONS;

    int i = 1;
    while (i < argc){

        switch(phase){

            // parsing the options to see if the -L or -xdev flag is being used
            case OPTIONS:
            
                if (argv[i][0] == '-'){

                    if (strcmp(argv[i],"-L") == 0){
                        g_follow_links = true;
                    }
                    else if (strcmp(argv[i],"-xdev") == 0){
                        g_xdev = true;
                    }
                    else {
                        // printf("Incorrect usage: %s\n", argv[i]);
                        if (strcmp(argv[i], "--help") == 0){
                            print_usage(argv[0]);
                            exit(0);
                        }
                        // need to exit if it's not valid
                        else if (strcmp(argv[i], "-name") == 0 || strcmp(argv[i], "-type") == 0 || strcmp(argv[i], "-mtime") == 0 || strcmp(argv[i], "-size") == 0 || strcmp(argv[i], "-perm") == 0){
                            if (paths_count == 0){
                                paths_count = 1;
                                default_path = true;
                            }
                            phase = FILTERS;
                            i--;
                        }
                        else {
                            // printf("Incorrect usage: %s\n: ", argv[i]);
                            fprintf(stderr, "Incorrect usage: %s\n: ", argv[i]);
                            exit(1);
                        }
                    }
                }
                else{
                    phase = PATHS;
                    i--;
                }
                break;

            case PATHS:

                if (argv[i][0] != '-'){

                    if (beginning_path){
                        paths_start_idx = i;
                        beginning_path = false;
                    }

                    paths_count++;
                }
                else{

                    // I think I need to move this, come back to this later
                    // if (paths_count == 0){
                    //     paths_count = 1;
                    //     default_path = true;
                    // }
                    phase = FILTERS;
                    i--;
                }
                break;
            
            case FILTERS:

                if (beginning_filter){
                    filters_start_idx = i;
                    beginning_filter = false;
                }
                
                // TODO: need to check if the argument exists in order to increment
                if (strcmp(argv[i], "-name") == 0 && (i+1 < argc)){

                    if (argv[i+1][0] != '-'){
                        g_nfilters++;
                        // printf("incrementing filters count in -name\n");
                    }
                    else{
                        fprintf(stderr, "Incorrect filter usage: %s and %s (back to back)\n", argv[i], argv[i+1]);
                        exit(1);
                    }
                }
                else if (strcmp(argv[i], "-type") == 0 && (i+1 < argc)){

                    if (argv[i+1][0] != '-'){
                        g_nfilters++;
                        // printf("incrementing filters count in type\n");

                    }
                    else{
                        fprintf(stderr, "Incorrect filter usage: %s and %s (back to back)\n", argv[i], argv[i+1]);
                        exit(1);
                    }
                }
                else if (strcmp(argv[i], "-mtime") == 0 && (i+1 < argc)){
                    
                    if (argv[i+1][0] != '-'){
                        g_nfilters++;
                        // printf("incrementing filters count in -mtime\n");

                    }
                    else{
                        fprintf(stderr, "Incorrect filter usage: %s and %s (back to back)\n", argv[i], argv[i+1]);
                        exit(1);
                    }
                }
                else if (strcmp(argv[i], "-size") == 0 && (i+1 < argc)){

                    // TODO: make a comment here
                    if (argv[i+1][0] == '-'){

                        // now we know the next has to be a number, so I am checking the ascii value
                        int ascii_value = (int) argv[i+1][1];
                        if (ascii_value >= 48 && ascii_value < 58){
                            size_flag = true;
                        }

                        if (!size_flag){                        
                            fprintf(stderr, "Incorrect filter usage: %s and %s (back to back)\n", argv[i], argv[i+1]);
                            exit(1);
                        }
                        else {
                            // printf("incrementing filters count in -size\n");
                            g_nfilters++;
                        }
                    }
                    else{
                        g_nfilters++;
                    }
                }
                else if (strcmp(argv[i], "-perm") == 0 && (i+1 < argc)){
                    
                    if (argv[i+1][0] != '-'){
                        g_nfilters++;
                        // printf("incrementing filters count in -perm\n");

                    }
                    else{
                        fprintf(stderr, "Incorrect filter usage: %s and %s (back to back)\n: ", argv[i], argv[i+1]);
                        exit(1);
                    }
                }
                else if (argv[i][0] == '-' && !size_flag){
                    fprintf(stderr, "Incorrect filter usage for: %s\n: ", argv[i]);
                    exit(1);
                }
                break;
        }
        i++;
    }
    
    char **paths = malloc(paths_count * sizeof(char*));
    if (paths == NULL){
        perror("There was an error with malloc when allocating for paths");
        exit(1);
    }

    // if the path is not specified
    *npaths = paths_count;
    if (default_path){
        paths[0]= ".";
    }
    else {
        for (int p = 0; p < paths_count; p++){
            paths[p] = argv[paths_start_idx + p];
        }
    }

    // still need code for filter here
    if (g_nfilters != 0){

        g_filters = malloc(g_nfilters * sizeof(filter_t));
        if (g_filters == NULL){
            perror("There was an error mallocing for g_filters");
            free(paths);
            exit(1);
        }
    }

    int filter_idx = 0;
    while (filter_idx < g_nfilters){

        // stores the first instance of 
        filter_t filter_entry;
        char* kind = argv[filters_start_idx + (2*filter_idx)];
        if (strcmp(kind, "-name") == 0){
            filter_entry.kind = FILTER_NAME;
            filter_entry.filter.pattern = argv[filters_start_idx + (2*filter_idx) + 1];
            // printf("The pattern is: %s\n", filter_entry.filter.pattern);
        }
        else if (strcmp(kind, "-type") == 0){
            filter_entry.kind = FILTER_TYPE;
            filter_entry.filter.type_char = argv[filters_start_idx + (2*filter_idx) + 1][0];
            // printf("The type char is: %c\n", filter_entry.filter.type_char);
        }
        else if (strcmp(kind, "-mtime") == 0){
            filter_entry.kind = FILTER_MTIME;
            char* endptr;
            long mtime_days = strtol(argv[filters_start_idx + (2*filter_idx) + 1], &endptr, 10);
            if (*endptr != '\0'){
                perror("endptr is not pointing at '\0'\n");
                free(g_filters);
                free(paths);
                exit(1);
            }
            filter_entry.filter.mtime_days = mtime_days;
            // printf("The mtime days is: %ld\n", mtime_days);
        }
        else if (strcmp(kind, "-size") == 0){
            filter_entry.kind = FILTER_SIZE;
            char* whole_num = argv[filters_start_idx + (2*filter_idx) + 1];
            int num_len = strlen(whole_num);
            char num[num_len];
            
            char cmp = argv[filters_start_idx + (2*filter_idx) + 1][0];
            if (cmp == '+'){
                filter_entry.filter.size.size_cmp = SIZE_CMP_GREATER;
                memcpy(num, whole_num + 1, num_len - 1);
                num[num_len-1] = '\0';

                // TODO: need to have a checker for parse_size()
                filter_entry.filter.size.size_bytes = parse_size(num);
                // printf("It is greater than and ");
            }
            else if (cmp == '-'){
                filter_entry.filter.size.size_cmp = SIZE_CMP_LESS;
                memcpy(num, whole_num + 1, num_len - 1);
                num[num_len-1] = '\0';
                filter_entry.filter.size.size_bytes = parse_size(num);
                // printf("It is less than and ");

            }
            else {
                filter_entry.filter.size.size_cmp = SIZE_CMP_EXACT;
                filter_entry.filter.size.size_bytes = parse_size(whole_num);
                // printf("It is same than and ");
            }
            // printf("the size bytes is %ld\n", filter_entry.filter.size.size_bytes);
        }
        else if (strcmp(kind, "-perm") == 0){
            char* perm_number = argv[filters_start_idx + (2*filter_idx) + 1];
            if (strlen(perm_number) > 4){
                printf("Incorrect usage of perm number, max 4 digits: %s\n", perm_number);
                // free(paths);
                // free(g_filters);
                return NULL;
            }
            filter_entry.kind = FILTER_PERM;
            char* endptr;
            long perm_num = strtol(perm_number, &endptr, 8);
            if (*endptr != '\0'){
                free(paths);
                free(g_filters);
                perror("endptr is not pointing at '\0'\n for perm");
                exit(1);
            }
            filter_entry.filter.perm_mode = (mode_t) perm_num;     
            // printf("the permission is: %d\n", filter_entry.filter.perm_mode);       
        }

        g_filters[filter_idx] = filter_entry;
        filter_idx++;
    }

    return paths;
}


/* ------------------------------------------------------------------ */
/*  BFS traversal                                                      */
/* ------------------------------------------------------------------ */

// helper for ordering the names (for qsort)
int compare(const void* name1, const void* name2){

    const char* name1_ = *(const char**) name1;
    const char* name2_ = *(const char**) name2;

    return strcmp(name1_,name2_);
}


// helper function for actually reordering
char** sort_order(char* dir_name, int* path_count){

    // maybe need a NULL check for dir?
    int capacity = 8;
    DIR* dir;
    struct dirent *dp;

    // TODO: print the directory that can't be opened
    if (((dir = opendir(dir_name)) == NULL)){
        perror("Cannot open directory");
        return NULL;
    }

    // was thinking that a queue could work, but it doesn't work with 
    char** path_array = malloc(capacity * sizeof(char*));
    if (path_array == NULL){
        perror("there was an error mallocing in the helper function");
        return NULL;
    }

    while ((dp = readdir(dir)) != NULL){

        if ((strcmp(dp->d_name,".") != 0)  && (strcmp(dp->d_name,"..") != 0)){

            // need to realloc()
            if (*path_count >= (capacity)){
                capacity *= 2;
                char** temp_array = realloc(path_array, capacity * sizeof(char*));
                if (temp_array == NULL){
                    // TODO: throw some error
                    perror("There was an error reallocing");
                    free(path_array);
                    return NULL;
                }
                path_array = temp_array;
            }
             
            path_array[*path_count] = strdup(dp->d_name);
            (*path_count)++;
        }
    }

    qsort(path_array, *path_count, sizeof(char*), compare);
    closedir(dir);
    return path_array;
}


/*
 * TODO 4: Implement this function.
 *
 * Traverse the filesystem breadth-first starting from the given paths.
 * For each entry, check the filters and print matching paths to stdout.
 *
 * You must handle:
 *   - The -L flag: controls whether symlinks are followed. Think about
 *     when to use stat(2) vs lstat(2) and what that means for descending
 *     into directories.
 *   - The -xdev flag: do not descend into directories on a different
 *     filesystem than the starting path (compare st_dev values).
 *   - Cycle detection (only relevant with -L): a symlink can point back
 *     to an ancestor directory. Only symlinks can create cycles (the OS
 *     forbids hard links to directories). Use the dev_ino_t type defined
 *     above to track visited directories — real directories should always
 *     be descended into, but symlinks to already-visited directories
 *     should be skipped.
 *   - Errors: if stat or opendir fails, print a message to stderr
 *     and continue traversing. Do not exit.
 *
 * The provided queue library (queue.h) implements a generic FIFO queue.
 */
static void bfs_traverse(char **start_paths, int npaths) {
    /* TODO: Your implementation here */

    // NOTE: stat follows a symbolic link while stat does not
    queue_t q;
    queue_init(&q);

    int i = 0;
    while (i < npaths){

        // enqueue the starting directories
        char* start_path = strdup(start_paths[i]);
        int res_q = queue_enqueue(&q,start_path);
        if (res_q == -1){
            printf("error\n");
            queue_destroy(&q);
            exit(1);
        }

        i++;
    }

    while (!queue_is_empty(&q)){

        char* dir_name = (char*) queue_dequeue(&q);

        int path_count = 0;
        char** ordered_paths = sort_order(dir_name, &path_count);
        if (ordered_paths == NULL){
            //TODO: print some error
            queue_destroy(&q);
        }

        // need to loop through and print
        for (int i = 0; i < path_count; i++){

            // constructing the full path
            char buff[PATH_MAX];
            snprintf(buff, sizeof(buff), "%s/%s", dir_name, ordered_paths[i]);
            char* path_dup = strdup(buff);              

            struct stat sb;
            if (lstat(path_dup, &sb) == -1) {
                perror("lstat");
                exit(EXIT_FAILURE);
            }

            if (matches_all_filters(path_dup, &sb)){
                printf("%s\n", path_dup);
            }


            if (S_ISDIR(sb.st_mode)){
                queue_enqueue(&q, path_dup);
            }
            else{
                free(path_dup);
            }
            free(ordered_paths[i]);
        }

        free(dir_name);
        free(ordered_paths);
    }
    queue_destroy(&q);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    g_now = time(NULL);

    int npaths;
    // char **paths = parse_args(argc, argv, &npaths);

    // if (paths == NULL){
    //     // some error
    // }
    
    // for (int i = 0; i < npaths; i++){

    //     printf("here is the path: %s and here is the npath count: %d\n", paths[i], npaths);
    // }

    // for (int j = 0; j < g_nfilters; j++){

    //     printf("here is the name: %s and here is the g_nfilters count: %d\n", g_filters[j].filter.pattern, g_nfilters);
    // }
    // printf("the number of filters is %d\n", g_nfilters);


    // filter_t filter = {0};
    // filter.kind = FILTER_NAME;
    // filter.filter.pattern = "*.c";
    
    // filter.filter.size.size_bytes = parse_size("1232");
    // filter.filter.size.size_cmp = SIZE_CMP_LESS;
    // filter.kind = FILTER_SIZE;

    // char* perm_number = "0644"; 
    // char* endptr;

    // long perm_num = strtol(perm_number, &endptr, 8);
    // filter.filter.perm_mode = perm_num;
    // filter.kind = FILTER_PERM;
    // struct stat sb = {0};
    // char* file = "testfile.txt";
    char** paths = parse_args(argc, argv, &npaths);

    bfs_traverse(paths, npaths);

    // printf("the path is %s\n", paths[0]);
    // g_nfilters = 5;
    // lstat(paths[0],&sb);
    // printf("Info from lstat:\n\ndir: %d (1 is yes and 0 is no)\nThe time is %ld\nThe size is %ld\nThe perm is %o\n\n", S_ISDIR((&sb)->st_mode), (&sb)->st_mtim.tv_sec, (&sb)->st_size, ((&sb)->st_mode & 0777));
    // bool res = matches_all_filters(paths[0], &sb);

    // if (res == true){
    //     printf("IT WORKS\n");
    // }

    // // debugging parse_size()
    // off_t num = parse_size(argv[1]);
    // printf("num of arguments: %d: \n", argc);
    // printf("This is the size before: %s and after: %ld\n", argv[1], num);

    free(paths);
    free(g_filters);
    return 0;
}
