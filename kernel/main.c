#include "print.h"
#include "init.h"
#include "debug.h"

int main() {
    put_str("my_os's kernel start\n");
    init_all();
    ASSERT(1==2);
    while(1);
    return 0;
}

