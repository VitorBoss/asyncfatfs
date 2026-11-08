"""Check Doxygen coverage and optional token equivalence to the former split source."""
import argparse
import hashlib
import json
import re
from pathlib import Path


FUNCTION = re.compile(
    r'(?m)^(?P<prefix>[A-Za-z_][\w \t*]*[ \t*])(?P<name>[A-Za-z_]\w*)'
    r'\s*\((?P<params>[^;{}]*?)\)\s*(?P<tail>[{#])')
INCLUDE = re.compile(r'(?m)^#include "(asyncfatfs_\w+\.inc)"[ \t]*$')
LEXER = re.compile(
    r'(?P<comment>/\*[\s\S]*?\*/|//[^\n]*)|'
    r'(?P<string>"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\')|'
    r'(?P<space>\s+)|(?P<token>[A-Za-z_]\w*|\d+(?:\.\d*)?|'
    r'>>=|<<=|\.\.\.|##|->|\+\+|--|&&|\|\||<=|>=|==|!=|<<|>>|[+*/%&|^\-]=|.)')


def expand(path):
    """Expand only the former library fragments, preserving feature directives."""
    source = path.read_text(encoding='utf-8-sig')
    return INCLUDE.sub(lambda match: expand(path.parent / match[1]).rstrip() + '\n', source)


def tokens(source):
    """Ignore documentation and spacing while preserving C strings and operators."""
    return [match[0] for match in LEXER.finditer(source) if match.lastgroup in ('string', 'token')]


def directives(source):
    """Preserve preprocessing line boundaries as well as the ordinary C token stream."""
    cleaned = LEXER.sub(
        lambda match: '\n' * match[0].count('\n') if match.lastgroup == 'comment' else match[0], source)
    cleaned = cleaned.replace('\\\n', '')
    return [tokens(line) for line in cleaned.splitlines() if line.lstrip().startswith('#')]


def preceding_document(source, start):
    """Find the declaration comment across supported visibility and compiler decorations."""
    position = start
    decoration = re.compile(r'^(?:#(?:if|else|endif)|__attribute__|__declspec|__forceinline|'
                            r'ONLY_EXPOSE_FOR_TESTING|AFATFS_WEAK|AFATFS_INLINE)')
    while position > 0:
        previous = source.rfind('\n', 0, max(0, position - 1)) + 1
        line = source[previous:position].strip()
        if line and not decoration.match(line):
            break
        position = previous
    prefix = source[:position].rstrip()
    assert prefix.endswith('*/'), f'Function at offset {start} has no adjacent Doxygen block'
    beginning = prefix.rfind('/**')
    assert beginning >= 0 and prefix.rfind('/*') == beginning
    return prefix[beginning:]


def parameter_names(text):
    """Extract the named parameters used by this implementation's C declarations."""
    text = re.sub(r'AFATFS_FAT_MIRROR_STATE\(([^()]*)\)', r'\1,', text)
    if text.strip() in ('', 'void'):
        return []
    names = []
    for declaration in text.split(','):
        if declaration.strip() == '...':
            names.append('...')
            continue
        pointer = re.search(r'\(\s*\*\s*(\w+)\s*\)', declaration)
        names.append(pointer[1] if pointer else re.findall(
            r'\b[A-Za-z_]\w*\b', re.sub(r'\[[^]]*\]', '', declaration))[-1])
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('lib/asyncfatfs.c'))
    parser.add_argument('--baseline', type=Path, help='Former lib directory containing asyncfatfs.c and its fragments')
    parser.add_argument('--output', type=Path)
    arguments = parser.parse_args()
    source = arguments.source.read_text(encoding='utf-8-sig')
    assert not INCLUDE.search(source), 'Production source still includes an implementation fragment'
    functions = []
    for match in FUNCTION.finditer(source):
        if match['tail'] == '#' and match['name'] not in ('afatfs_flushCache', 'afatfs_flushCacheFiltered'):
            continue
        comment = preceding_document(source, match.start())
        assert '@brief ' in comment, match['name']
        expected = parameter_names(match['params'])
        documented = re.findall(r'@param(?:\[[^]]*\])?\s+(\S+)', comment)
        assert documented == expected, (match['name'], expected, documented)
        result_type = re.sub(r'\b(?:static|inline|AFATFS_INLINE|AFATFS_WEAK|ONLY_EXPOSE_FOR_TESTING)\b', '', match['prefix']).strip()
        assert ('@return ' in comment) == (result_type != 'void'), match['name']
        functions.append(match['name'])
    assert functions
    result = dict(source=str(arguments.source), source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                  documented_definitions=len(functions), unique_function_names=len(set(functions)),
                  optional_include_count=0, functions=functions)
    if arguments.baseline:
        baseline = expand(arguments.baseline / 'asyncfatfs.c')
        before = tokens(baseline)
        after = tokens(source)
        assert before == after, next((index, old, new) for index, (old, new) in
                                      enumerate(zip(before, after)) if old != new) if len(before) == len(after) else (
                                          'Token counts differ', len(before), len(after))
        assert directives(baseline) == directives(source), 'Preprocessor directives changed'
        result.update(executable_tokens_unchanged=True, preprocessor_directives_unchanged=True,
                      token_count=len(after), baseline=str(arguments.baseline))
    if arguments.output:
        arguments.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'functions'}, indent=2))


if __name__ == '__main__':
    main()
