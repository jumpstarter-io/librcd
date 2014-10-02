#include "rest.h"

#pragma librcd


fstr_mem_t* rest_basic_auth_val(fstr_t username, fstr_t password) { sub_heap {
    return escape(conc("Basic ", fss(fstr_base64_encode(concs(username, ":", password)))));
}}

fstr_mem_t* rest_urlencode(fstr_t str, bool plus_enc_sp) {
    fstr_mem_t* buf = fstr_alloc(str.len * 3);
    fstr_t buf_tail = fss(buf);
    for (size_t i = 0; i < str.len; i++) {
        uint8_t ch = str.str[i];
        if ((ch >= 0 && ch <= 9)
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z')
        || (ch == '-')
        || (ch == '.')
        || (ch == '_')
        || (ch == '~')) {
            // Copy over raw character.
            fstr_putc(&buf_tail, ch);
        } else if (plus_enc_sp && ch == ' ') {
            // Plus encode space.
            fstr_putc(&buf_tail, '+');
        } else {
            // Hexencode the character.
            fstr_putc(&buf_tail, '%');
            fstr_serial_uint(fstr_slice(buf_tail, 0, 2), ch, 16);
            buf_tail = fstr_slice(buf_tail, 2, -1);
        }
    }
    // Throw away the unwritten length of the buffer and return it.
    buf->len = fstr_detail(fss(buf), buf_tail).len;
    return buf;
}

static inline bool is_hex(uint8_t ch) {
    return
        ('0' <= ch && ch <= '9') ||
        ('A' <= ch && ch <= 'F') ||
        ('a' <= ch && ch <= 'f');
}

static inline uint32_t hex_to_int(uint8_t ch) {
    if ('0' <= ch && ch <= '9') return ch - '0';
    if ('A' <= ch && ch <= 'F') return ch - 'A' + 10;
    if ('a' <= ch && ch <= 'f') return ch - 'a' + 10;
    unreachable();
}

fstr_mem_t* rest_urldecode(fstr_t str, bool plus_dec_sp) {
    fstr_mem_t* out = fstr_alloc(str.len);
    size_t out_i = 0;
    for (size_t i = 0; i < str.len; i++) {
        uint8_t ch;
        if (str.str[i] == '%' && i + 2 < str.len && is_hex(str.str[i + 1]) && is_hex(str.str[i + 2])) {
            ch = hex_to_int(str.str[i + 1]) * 16 + hex_to_int(str.str[i + 2]);
            i += 2;
        } else if (plus_dec_sp && str.str[i] == '+') {
            ch = ' ';
        } else {
            ch = str.str[i];
        }
        out->str[out_i] = ch;
        out_i++;
    }
    out->len = out_i;
    return out;
}

fstr_mem_t* rest_url_query_encode(dict(fstr_t)* url_params) {
    if (dict_count(url_params, fstr_t) == 0)
        return fstr_cpy("");
    sub_heap {
        list(fstr_t)* parts = new_list(fstr_t);
        bool first = true;
        dict_foreach(url_params, fstr_t, key, value) {
            if (first) {
                first = false;
            } else {
                list_push_end(parts, fstr_t, "&");
            }
            list_push_end(parts, fstr_t, fss(rest_urlencode(key, false)));
            list_push_end(parts, fstr_t, "=");
            list_push_end(parts, fstr_t, fss(rest_urlencode(value, false)));
        }
        return escape(fstr_implode(parts, ""));
    }
}

dict(fstr_t)* rest_url_query_decode(fstr_t url_query) {
    dict(fstr_t)* url_params = new_dict(fstr_t);
    for (fstr_t param; fstr_iterate_trim(&url_query, "&", &param);) {
        fstr_t enc_key, enc_value;
        if (!fstr_divide(param, "=", &enc_key, &enc_value)) {
            enc_key = param;
            enc_value = "";
        }
        fstr_t key = fss(rest_urldecode(enc_key, true));
        fstr_t value = fss(rest_urldecode(enc_value, true));
        dict_replace(url_params, fstr_t, key, value);
    }
    return url_params;
}

static fstr_t header_line(fstr_t key, fstr_t val) {
    return sconc(key, ": ", val, "\r\n");
}

fstr_mem_t* rest_serialize_request(rest_req_t request) { sub_heap {
    list(fstr_t)* req_lines = new_list(fstr_t);
    list_push_end(req_lines, fstr_t, sconc(request.method, " ", request.path, " HTTP/1.1\r\n"));
    bool host_set = false;
    if (request.headers != 0) {
        dict_foreach(request.headers, fstr_t, key, val) {
            if (fstr_equal(fss(fstr_lower(key)), "host"))
                host_set = true;
            list_push_end(req_lines, fstr_t, header_line(key, val));
        }
    }
    if (!host_set)
        list_push_end(req_lines, fstr_t, header_line("host", request.host));
    if (request.body.len > 0)
        list_push_end(req_lines, fstr_t, header_line("content-length", ui2fs(request.body.len)));
    list_push_end(req_lines, fstr_t, "\r\n");
    list_push_end(req_lines, fstr_t, request.body);
    return escape(fstr_implode(req_lines, ""));
}}

fstr_t rest_read_response(rio_t* rio_r, size_t max_size) { sub_heap {
    fstr_mem_t* header_buf = fstr_alloc(8192);
    bool has_chunked = false;
    bool has_content_length = false;
    size_t content_length;
    fstr_t head = rio_read_to_separator(rio_r, "\r\n\r\n", fss(header_buf));
    list(fstr_t)* head_lines = fstr_explode(head, "\r\n");
    fstr_t status_line = fss(fstr_lower(fstr_trim(list_pop_start(head_lines, fstr_t))));
    if (!fstr_equal(status_line, "http/1.1 200 ok"))
        throw("request failed", exception_io);
    list_foreach(head_lines, fstr_t, line) {
        list(fstr_t)* header = fstr_explode(line, ": ");
        fstr_t header_key = fss(fstr_lower(fstr_trim(list_pop_start(header, fstr_t))));
        fstr_t header_val = fstr_trim(list_pop_start(header, fstr_t));
        if (fstr_equal(header_key, "content-length")) {
            has_content_length = true;
            content_length = fs2ui(header_val);
            continue;
        } else if (fstr_equal(header_key, "transfer-encoding")
            && fstr_equal(header_val, "chunked" )) {
            has_chunked = true;
            continue;
        }
    }
    fstr_mem_t* body_buffer;
    fstr_t body;
    if (has_chunked) {
        body_buffer = fstr_alloc(max_size);
        size_t body_len = 0;
        fstr_t body_buffer_tail = fss(body_buffer);
        // Chunks should be less than 4GB, if turns out to be a problem we will
        // up this limit later.
        fstr_mem_t* chunk_size_buffer = fstr_alloc(8);
        bool more_chunks = true;
        while (more_chunks) {
            fstr_t chunk_size_str = rio_read_to_separator(rio_r, "\r\n", fss(chunk_size_buffer));
            size_t c_size = chunk_size_str.len == 0? 0: fstr_to_uint(chunk_size_str, 16);
            if (c_size == 0) {
                fstr_mem_t* end_buffer = fstr_alloc(2);
                fstr_t end = rio_read(rio_r, fss(end_buffer));
                if (fstr_equal(end, "\r\n"))
                    break;
                else
                    throw_eio("invalid HTTP response", rest);
            }
            if (c_size > (body_buffer_tail.len + 2))
                throw_eio("too big response", rest);
            fstr_t chunk = rio_read_to_separator(rio_r, "\r\n", fstr_slice(body_buffer_tail, 0, c_size +2));
            assert(c_size == chunk.len);
            body_len += c_size;
        }
        body = fstr_slice(fss(body_buffer), 0, body_len);
    } else if (has_content_length) {
        body_buffer = fstr_alloc(content_length);
        body = fss(body_buffer);
        rio_read_fill(rio_r, body);
    } else
        throw_eio("invalid HTTP response", rest);
    escape_list(body_buffer);
    return body;
}}

fstr_t rest_call(rio_t* rio_h, rest_req_t request, size_t max_response_size) {
    rio_write(rio_h, fss(rest_serialize_request(request)));
    return rest_read_response(rio_h, max_response_size);
}
