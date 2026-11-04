#include "access_contract.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(jetkvm_access_lifetime_ticks(7*24*60*60, 100) == 60480000);
    assert(jetkvm_access_lifetime_ticks(7*24*60*60, 1000) == 604800000);
    assert(jetkvm_access_has_encoded_nul("{\"password\":\"abc\\u0000def\"}"));
    assert(!jetkvm_access_has_encoded_nul("{\"password\":\"abc\\\\u0000def\"}"));
    assert(!jetkvm_access_has_encoded_nul("{\"password\":\"abc\\u005cu0000def\"}"));
    assert(jetkvm_access_has_encoded_nul("{\"password\":\"abc\\\\\\u0000def\"}"));
    assert(!jetkvm_access_has_encoded_nul("{\"password\":\"abc\\u0001def\"}"));
    puts("Password JSON escapes and seven-day token lifetime: OK");
}
