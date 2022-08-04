/**
 *  Title       : mmv-c
 *  Description : interactively move files and directories
 *  Author      : Jacob M. Penney
 */

#include "mmv.h"

// TODO:
// int return values should all match. Some functions
// return 0 for success while others return 1

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "mmv: missing file operand\n");
        exit(EXIT_FAILURE);
    }

    struct Map *rename_map = make_str_hashmap(argv, argc);
    if (rename_map == NULL)
        goto free_map_out;

    char tmp_path[] = "/tmp/mmv_XXXXXX";

    umask(077);

    if (write_strarr_to_tmpfile(rename_map, tmp_path) != 0)
        goto rm_path_out;

    if (open_file_in_editor(tmp_path) != 0)
        goto rm_path_out;

    if (rename_filesystem_items(rename_map, tmp_path) != 0)
        goto rm_path_out;

    rm_path(tmp_path);
    free_hashmap(rename_map);
    return EXIT_SUCCESS;

rm_path_out:
    rm_path(tmp_path);

free_map_out:
    free_hashmap(rename_map);
    return EXIT_FAILURE;
}

struct Map *make_str_hashmap(char *names[], int arg_count)
{
    unsigned int i;
    const unsigned int u_arg_count = (unsigned int)arg_count;

    // (6 * (val - 1)) + 1 is a formula that creates a
    // prime number using some value as a base. We are
    // using this to create a prime hash map size to
    // attempt to mitigate collisions. While the FNV
    // hash is known to be of high quality and this is
    // supposed to not be necessary, this is not heavy
    // to implement and adds a layer of protection.
    const unsigned int map_space = (6 * (u_arg_count - 1)) + 1;

    struct Map *map = alloc_hashmap(u_arg_count, map_space);
    if (map == NULL)
    {
        perror("mmv: failed to allocate memory to initialize hashmap");
        exit(EXIT_FAILURE);
    }

    // start at 1 to avoid reading program invocation
    for (i = 1; i < u_arg_count; i++)
        if (hashmap_insert_str(map_space, names[i], map) == 0)
            return NULL;

    return map;
}

struct Map *alloc_hashmap(const unsigned int num_names, const unsigned int map_size)
{
    unsigned int i;

    struct Map *map = malloc(sizeof(struct Map) + (num_names - 1) * sizeof(int));
    if (map == NULL)
        return NULL;

    map->hashmap = malloc(sizeof(char *) * map_size);
    if (map->hashmap == NULL)
    {
        free(map);
        return NULL;
    }

    map->num_keys = 0;

    for (i = 0; i < map_size; i++)
        map->hashmap[i] = NULL;

    return map;
}

int hashmap_insert_str(const unsigned int map_space, char *cur_str, struct Map *map)
{
    int dupe_found = -1;
    unsigned int hash = hash_str(cur_str, map_space);

    while (map->hashmap[hash] != NULL && dupe_found != 0)
    {
        // strcmp returns 0 if strings are identical
        dupe_found = strcmp(map->hashmap[hash], cur_str);

        hash = (hash + 1 < map_space) ? hash + 1 : 0;
    }
    if (dupe_found == 0)
        return 0;

    if (cpy_str_to_arr(&map->hashmap[hash], cur_str) == NULL)
        return 0;

    map->keyarr[map->num_keys] = hash;
    map->num_keys++;

    return 1;
}

unsigned int hash_str(char *str, const unsigned int map_size)
{
    unsigned char *s = (unsigned char *)str;
    Fnv32_t hval = ((Fnv32_t)0x811c9dc5);

    while (*s)
    {
        hval ^= (Fnv32_t)*s++;
        hval += (hval << 1) + (hval << 4) + (hval << 7) + (hval << 8) + (hval << 24);
    }

    return hval % map_size;
}

char *cpy_str_to_arr(char **arr_dest, const char *src_str)
{
    *arr_dest = malloc((strlen(src_str) + 1) * sizeof(char));
    if (arr_dest == NULL)
    {
        perror("mmv: failed to allocate memory for new map node source str");
        return NULL;
    }

    return strcpy(*arr_dest, src_str);
}

int write_strarr_to_tmpfile(struct Map *map, char tmp_path_template[])
{
    size_t i;

    FILE *tmp_fptr = open_tmp_path_fptr(tmp_path_template);
    if (tmp_fptr == NULL)
    {
        fprintf(stderr, "mmv: failed to open \"%s\": %s\n", tmp_path_template, strerror(errno));
        return errno;
    }

    for (i = 0; i < map->num_keys; i++)
        fprintf(tmp_fptr, "%s\n", map->hashmap[map->keyarr[i]]);

    fclose(tmp_fptr);

    return 0;
}

FILE *__attribute__((malloc)) open_tmp_path_fptr(char *tmp_path)
{
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd == -1)
        return NULL;

    return fdopen(tmp_fd, "w");
}

int open_file_in_editor(const char *path)
{
    int ret;
    char *editor_name = getenv("EDITOR");
    if (editor_name == NULL)
        editor_name = "nano";

    // provide space for "$EDITOR path\0", e.g. "nano test.txt\0"
    const size_t cmd_len = strlen(editor_name) + strlen(path) + 2;

    char *edit_cmd = malloc(sizeof(edit_cmd) * cmd_len);
    if (edit_cmd == NULL)
    {
        perror("mmv: failed to allocate memory for $EDITOR command string");
        return errno;
    }

    ret = snprintf(edit_cmd, cmd_len, "%s %s", editor_name, path);
    if (ret < 0 || ret > (int)cmd_len)
    {
        perror("mmv: couldn't create $EDITOR command string");
        return errno;
    }

    // open temporary file containing argv using editor of choice
    if (system(edit_cmd) != 0)
    {
        fprintf(stderr, "mmv: \'%s\' returned non-zero exit status: %d\n", editor_name, ret);
        return errno;
    }

    free(edit_cmd);

    return 0;
}

int rename_filesystem_items(struct Map *map, char path[])
{
    char cur_str[PATH_MAX], *read_ptr = "";
    unsigned int *keyarr = map->keyarr;
    size_t i = 0;

    FILE *tmp_fptr = fopen(path, "r");
    if (tmp_fptr == NULL)
    {
        fprintf(stderr, "mmv: failed to open \"%s\" in \"r\" mode: %s\n", path, strerror(errno));
        return errno;
    }

    while (read_ptr != NULL && i < map->num_keys)
    {
        read_ptr = fgets(cur_str, PATH_MAX, tmp_fptr);

        if (read_ptr != NULL && strcmp(cur_str, "\n") != 0)
        {
            cur_str[strlen(cur_str) - 1] = '\0';
            rename_path(map->hashmap[keyarr[i]], cur_str);
            i++;
        }
    }

    fclose(tmp_fptr);

    return 0;
}

void rename_path(const char *src, const char *dest)
{
    if (rename(src, dest) == -1)
        fprintf(stderr, "mmv: \'%s\' to \'%s\': %s\n", src, dest, strerror(errno));
}

void rm_path(char *path)
{
    if (remove(path) == -1)
        fprintf(stderr, "mmv: failed to delete \'%s\': %s\n", path, strerror(errno));
}

void free_hashmap(struct Map *map)
{
    size_t i;
    unsigned int key;

    for (i = 0; i < map->num_keys; i++)
    {
        key = map->keyarr[i];
        free(map->hashmap[key]);
    }

    free(map->hashmap);
    free(map);
}
