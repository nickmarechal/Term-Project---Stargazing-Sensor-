#ifndef SUITES_H
#define SUITES_H

/* One entry point per test file. test_main.c calls them in order. */
void test_crc32(void);
void test_record(void);
void test_store(void);

#endif
