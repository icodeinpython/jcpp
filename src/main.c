#include "jcpp.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *input = NULL;
    if (argc >= 2) {
        input = argv[1];
    } else {
        fprintf(stderr, "Usage: jcpp <input>\n");
        return 1;
    }

    MacroTable table;
    IncludeStack stack;
    TokenList output = {0};

    macro_table_init(&table);
    include_stack_init(&stack);

    preprocess_file(input, &output, &table, &stack);

    for (int i = 0; i < output.count; i++) {
        fputs(output.data[i].text, stdout);
    }

    token_list_free(&output);
    include_stack_free(&stack);
    macro_table_free(&table);

    return 0;
}
