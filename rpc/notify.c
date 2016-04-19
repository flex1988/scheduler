#include "notify.pb-c.h"

int main()
{
    Notify__Test msg = NOTIFY__TEST__INIT;
    msg.program = 1001;
    msg.version = 1002;
    msg.method = 1003;
    msg.str = "hello world";
    int len = notify__test__get_packed_size(&msg);
    printf("size of student info : %u\n", len);
}
