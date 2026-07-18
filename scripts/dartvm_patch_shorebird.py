#!/usr/bin/python3
import os
import sys


GUARD = '#if defined(DART_SHOREBIRD)'


def patch_app_snapshot(source_file: str):
    with open(source_file, 'r', encoding='utf-8') as f:
        code = f.read()

    if GUARD in code:
        return

    start = '''    predefined_start_index_ = d->next_index();
'''
    replacement_start = '''#if defined(DART_SHOREBIRD)
    // Shorebird snapshots prefix the Class cluster with the highest class ID
    // so patched releases can reserve the class-table slot before reading it.
    const intptr_t last_cid = d->ReadUnsigned();
#endif
    predefined_start_index_ = d->next_index();
'''
    table = '''    ClassTable* table = d->isolate_group()->class_table();
    for (intptr_t i = 0; i < count; i++) {
'''
    replacement_table = '''    ClassTable* table = d->isolate_group()->class_table();
#if defined(DART_SHOREBIRD)
    table->AllocateIndex(last_cid);
#endif
    for (intptr_t i = 0; i < count; i++) {
'''

    if code.count(start) != 1 or code.count(table) != 1:
        raise RuntimeError(
            'Unsupported Dart app_snapshot.cc layout; cannot apply Shorebird reader patch'
        )

    code = code.replace(start, replacement_start, 1)
    code = code.replace(table, replacement_table, 1)
    with open(source_file, 'w', encoding='utf-8') as f:
        f.write(code)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f'usage: {sys.argv[0]} <dart-sdk-directory>')
    source_file = os.path.join(sys.argv[1], 'runtime', 'vm', 'app_snapshot.cc')
    patch_app_snapshot(source_file)


if __name__ == '__main__':
    main()
