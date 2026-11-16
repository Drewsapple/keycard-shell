#include "unity.h"
#include "ethereum/eth_multisend.h"
#include "ethereum/eth_data.h"
#include "storage/fs.h"
#include "hal_host.h"
#include "ui/i18n.h"
#include <string.h>

static uint8_t data[4096];
void setUp(void) { memset(data, 0, sizeof(data)); hal_host_flash_reset(); i18n_set_strings(i18n_english_strings); }
void tearDown(void) {}

static void word(uint8_t* p, size_t v) {
  for (int i = 31; i >= 0; i--) { p[i] = v & 255; v >>= 8; }
}

/* Independent encoder for the packed records inside multiSend(bytes). */
static size_t fixture(unsigned count, size_t child_len) {
  memset(data, 0, sizeof(data));
  for (unsigned i = 0; i < count; i++) {
    uint8_t* p = data + i * (85 + child_len);
    memset(p + 1, i + 1, 20);
    p[21] = 0x80; p[52] = i;
    word(p + 53, child_len);
    memset(p + 85, 0xab, child_len);
  }
  return count * (85 + child_len);
}

static void valid(void) {
  for (unsigned count = 1; count <= 32; count++) {
    for (size_t child_len = 0; child_len < 5; child_len++) {
      size_t len = fixture(count, child_len);
      eth_multisend_t b;
      TEST_ASSERT_EQUAL(ETH_MULTISEND_COMPLETE, eth_multisend_init(&b, data, len));
      TEST_ASSERT_EQUAL(count, b.count);
      for (unsigned i = 0; i < count; i++) {
        eth_multisend_call_t c;
        TEST_ASSERT_TRUE(eth_multisend_next(&b, &c));
        TEST_ASSERT_EQUAL(0, c.operation);
        TEST_ASSERT_EQUAL(i + 1, c.to[0]);
        TEST_ASSERT_EQUAL(0x80, c.value[0]);
        TEST_ASSERT_EQUAL(i, c.value[31]);
        TEST_ASSERT_EQUAL(child_len, c.data_len);
        if (child_len) TEST_ASSERT_EQUAL(0xab, c.data[child_len - 1]);
      }
      eth_multisend_call_t c;
      TEST_ASSERT_FALSE(eth_multisend_next(&b, &c));
    }
  }
}

static void invalid(void) {
  eth_multisend_t b;
  size_t len = fixture(2, 0);
  data[85] = 2;
  TEST_ASSERT_EQUAL(ETH_MULTISEND_MALFORMED, eth_multisend_init(&b, data, len));
  TEST_ASSERT_EQUAL(0, b.count); /* Do not expose the valid prefix. */
  data[85] = 1;
  TEST_ASSERT_EQUAL(ETH_MULTISEND_COMPLETE, eth_multisend_init(&b, data, len));
  data[53] = 1; /* Full-width uint256 length cannot fit. */
  TEST_ASSERT_EQUAL(ETH_MULTISEND_MALFORMED, eth_multisend_init(&b, data, len));
  fixture(1, 0);
  TEST_ASSERT_EQUAL(ETH_MULTISEND_MALFORMED, eth_multisend_init(&b, data, 86));
}

static void limits(void) {
  eth_multisend_t b;
  size_t len = fixture(17, 0);
  TEST_ASSERT_EQUAL(ETH_MULTISEND_COMPLETE, eth_multisend_init(&b, data, len));
  TEST_ASSERT_EQUAL(17, b.count);
  TEST_ASSERT_EQUAL(ETH_MULTISEND_EMPTY, eth_multisend_init(&b, data, 0));
  len = fixture(1, 3);
  for (size_t truncated = 1; truncated < len; truncated++)
    TEST_ASSERT_EQUAL(ETH_MULTISEND_MALFORMED, eth_multisend_init(&b, data, truncated));
}

static void random_inputs(void) {
  uint32_t seed = 1234567;
  eth_multisend_t b;
  for (unsigned i = 0; i < 10000; i++) {
    seed = seed * 1664525 + 1013904223;
    size_t len = seed % sizeof(data);
    for (size_t j = 0; j < len; j++) {
      seed = seed * 1664525 + 1013904223; data[j] = seed >> 24;
    }
    eth_multisend_init(&b, data, len);
  }
}

static void formatting(void) {
  /* Same relative-offset ABI representation as the installed database. */
  static struct {
    fs_entry_t entry;
    eth_abi_function_t function;
    eth_abi_argument_t argument;
    char name[10];
  } abi;
  memset(&abi, 0, sizeof(abi));
  abi.entry.magic = FS_ABI_MAGIC;
  abi.entry.len = sizeof(abi) - sizeof(fs_entry_t);
  abi.function.selector = ETH_DATA_MULTISEND_SELECTOR;
  abi.function.ext_selector = ETH_DATA_MULTISEND_EXT_SELECTOR;
  abi.function.attrs = ETH_FUNC_PAYABLE;
  abi.function.first_arg = (uint8_t*)&abi.argument - (uint8_t*)&abi.function;
  abi.function.name = (uint8_t*)abi.name - (uint8_t*)&abi.function;
  abi.argument.type = ETH_ABI_VARBYTES;
  strcpy(abi.name, "multiSend");
  static uint8_t encoded[512], out[1024];
  memset(encoded, 0, sizeof(encoded));
  size_t length = fixture(2, 1);
  memcpy(encoded, "\x8d\x80\xff\x0a", 4);
  word(encoded + 4, 32);
  word(encoded + 36, length);
  memcpy(encoded + 68, data, length);
  size_t encoded_len = 68 + ((length + 31) / 32) * 32;
  TEST_ASSERT_NULL(eth_data_recognize(encoded, encoded_len, false));
  size_t out_len;
  eth_data_format(NULL, encoded, encoded_len, out, sizeof(out), &out_len);
  TEST_ASSERT_EQUAL_HEX8('8', out[0]); /* No ABI: raw calldata. */
  TEST_ASSERT_EQUAL(ERR_OK, fs_write(&abi.entry, sizeof(abi)));
  const eth_abi_function_t* found = eth_data_recognize(encoded, encoded_len, false);
  TEST_ASSERT_NOT_NULL(found);
  const uint8_t target[20] = {0x38,0x86,0x9b,0xf6,0x6a,0x61,0xcf,0x6b,0xdb,0x99,0x6a,0x6a,0xe4,0x0d,0x58,0x53,0xfd,0x43,0xb5,0x26};
  const uint8_t target150[20] = {0x21,0x85,0x43,0x28,0x80,0x04,0xcd,0x07,0x83,0x24,0x72,0xd4,0x64,0x64,0x81,0x73,0xc7,0x7d,0x7e,0xb7};
  const uint8_t callonly150[20] = {0xa8,0x3c,0x33,0x6b,0x20,0x40,0x1a,0xf7,0x73,0xb6,0x21,0x9b,0xa5,0x02,0x71,0x74,0x33,0x8d,0x18,0x36};
  const uint8_t caller[20] = {0x33};
  eth_data_context_t context = {.to = target, .caller = caller, .operation = 1};
  eth_data_format(found, encoded, encoded_len, out, sizeof(out), &out_len);
  TEST_ASSERT_NULL(strstr((char*)out, "Call 1/2")); /* Missing context is opaque. */
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NOT_NULL(strstr((char*)out, "Call 1/2"));
  TEST_ASSERT_NOT_NULL(strstr((char*)out, "Call 2/2"));
  TEST_ASSERT_EQUAL_HEX8('b', out[out_len - 1]); /* No trailing blank pages. */
  context.to = caller;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NULL(strstr((char*)out, "Call 1/2")); /* ABI alone does not bind a target. */
  context.to = target;
  context.operation = 0;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NULL(strstr((char*)out, "Call 1/2")); /* MultiSend requires delegatecall. */
  context.operation = 1;
  /* Payable recognition must also allow the batch expansion. */
  found = eth_data_recognize(encoded, encoded_len, true);
  TEST_ASSERT_NOT_NULL(found);
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NOT_NULL(strstr((char*)out, "Call 1/2"));
  memset(encoded + 69, 0, 20);
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NOT_NULL(strstr((char*)out, "To: 0x0000000000000000000000000000000000000000"));
  context.to = target150;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NOT_NULL(strstr((char*)out, "To: 0x3300000000000000000000000000000000000000"));
  context.caller = NULL;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NULL(strstr((char*)out, "Call 1/2"));
  context.to = callonly150;
  context.operation = 0;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NOT_NULL(strstr((char*)out, "To: 0xa83c336b20401af773b6219ba5027174338d1836"));
  encoded[68 + 86] = 1;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NULL(strstr((char*)out, "Call 1/2")); /* CallOnly forbids inner delegatecall. */
  context.to = target150;
  context.caller = caller;
  context.operation = 1;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NOT_NULL(strstr((char*)out, "Warning: nested delegatecall"));
  /* A malformed last record must discard the valid first call's expansion. */
  encoded[68 + 86] = 2;
  eth_data_format_with_context(found, encoded, encoded_len, out, sizeof(out), &out_len, &context);
  TEST_ASSERT_NULL(strstr((char*)out, "Call 1/2"));
  eth_data_format_with_context(found, encoded, encoded_len, out, 4, &out_len, &context);
  TEST_ASSERT_EQUAL(0, out_len);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(valid); RUN_TEST(invalid); RUN_TEST(limits); RUN_TEST(random_inputs); RUN_TEST(formatting);
  return UNITY_END();
}
