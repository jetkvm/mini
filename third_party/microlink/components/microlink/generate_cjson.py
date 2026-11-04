"""Keep exact large integer tokens in the client's private cJSON instance."""
from pathlib import Path
import sys

source, target = map(Path, sys.argv[1:])
text = source.read_text()
before = """    input_buffer->offset += (size_t)(after_end - number_c_string);
    /* free the temporary buffer */
    input_buffer->hooks.deallocate(number_c_string);
    return true;"""
after = """    input_buffer->offset += (size_t)(after_end - number_c_string);
    /* Historical Tailscale NodeIDs can exceed the exact range of a double.
     * cJSON_Delete already frees valuestring for numbers, and Duplicate
     * preserves it. Other numbers keep the original allocator behavior. */
    if (number > 9007199254740991.0 && !has_decimal_point &&
        after_end == number_c_string + number_string_length)
    {
        item->valuestring = (char *)number_c_string;
    }
    else
    {
        input_buffer->hooks.deallocate(number_c_string);
    }
    return true;"""
if text.count(before) != 1:
    raise SystemExit("Private cJSON integer-token patch no longer matches the managed source")
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(text.replace(before, after))
