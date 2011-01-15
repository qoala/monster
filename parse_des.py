#!/usr/bin/env python
"""
usage: parse_des.py [des_folder] [output_file] [options]

DESCRIPTION
    Attempts to parse all of the .des files contained within des_folder,
    convert the output to C++, and store in output_file.

OPTIONS
    -v  --verbose   Print a list of generated files.
    -h  --help      Print this message.

DEFAULTS
    des_folder      %s
    output_file     %s
"""

import re, sys, os

# Defaults:
DEFAULT_DES_FOLDER = "crawl-ref/crawl-ref/source/dat/des"
DEFAULT_OUTPUT = "vault_monster_data.cc"

# These des files will be ignored.
IGNORE_DES_FILES = ["test.des"]

# Any des file in these subfolders will be ignored.
IGNORE_DES_SUBFOLDERS = ["builder", "zotdef", "tutorial"]

# If ever the need for cross-platform compatability...
DIR_DELIM = os.path.sep

# Pre-compiled regular expressions and associated functions:

# Return all vault-defined KMONS or MONS lines
FIND_MONS_LINES = re.compile("(K?MONS:\s*(?:[^\n]*)\n)")

# Replace multiple instances of whitespace with a single instance of whitespace.
_CLEANUP_WHITESPACE = re.compile("(\s)+")
CLEANUP_WHITESPACE = lambda line: re.sub(_CLEANUP_WHITESPACE, r"\1", line)

# Fix up whitespace for spells first: if whitespace isn't correctly stripped,
# the spells in the specification will have extraneous spaces and the spec
# won't be parsed properly.
_SPELL_CLEANUP = re.compile(";\\\n")
CLEANUP_SPELLS = lambda line: re.sub(_SPELL_CLEANUP, ";", line)

# Remove all escaped new lines.
CLEANUP_LINES = lambda line: line.replace("\\\n", "")

class MapParseError (Exception):
    """
    This exception is raised when an error is encountered while parsing maps.
    """
    pass

def cleanup_mons_line (line):
    """
    Remove "MONS" or "KMONS" specifiers and related information (glyphs, etc).

    :``line``: The line to clean up.
    """
    line = line.strip()

    if line.startswith("KMONS:"):
        line = line.split("=")[-1].strip()

    line = line.replace("MONS:", "")

    line = CLEANUP_WHITESPACE(line)

    return line.strip()

def parse_mons_line (line):
    """
    Return a list of monsters as contained in ``line``, cleaning up each monster.

    :``line``: The line to parse.
    """
    line = cleanup_mons_line(line)

    monsters = line.split("/")
    new_monsters = []

    for monster in monsters:
        if "," in monster:
            new_monsters.extend(monster.split(","))
        else:
            new_monsters.append(monster)

    return [cleanup_mons_line(mons) for mons in new_monsters]

def cull_unnamed_monsters (lines):
    """
    Return a copy of ``lines`` that does not contained any unnamed monsters.
    These are indistinguishable from other monsters.

    :``lines``: The list of monsters to parse.
    """
    new_monsters = []

    for mons in lines:
        if "name" not in mons:
            continue

        new_monsters.append(mons)

    return new_monsters

def generate_monster_lines (des_folder, cull=True, verbose=False):
    """
    Iterate over every .des file contained with ``des_folder`` and return a list
    of monsters as defined by MONS or KMONS specifiers. Files that are contained
    within the global variable IGNORE_DES_FILES will be ignored; likewise,
    folders in the global variable IGNORE_DES_SUBFOLDERS will be skipped.

    :``des_folder``: The folder to search. This search is performed recursively.
    :``cull``: If True, will only return named monsters.
    :``verbose``: If True, will note which files have been parsed.
    """
    done = []

    monster_lines = []

    for dirpath, dirnames, filenames in os.walk(des_folder):
        this_dir = dirpath.split(DIR_DELIM)[-1]

        if this_dir in IGNORE_DES_SUBFOLDERS:
            continue

        for fname in filenames:
            if fname in done:
                continue

            if fname in IGNORE_DES_FILES:
                continue

            if not fname.endswith(".des"):
                continue

            if verbose:
                print " GEN %s" % fname

            this_file = open(os.path.join(dirpath, fname))
            this_data = "\n".join([line.strip() for line in this_file.readlines()])
            this_file.close()

            this_data = CLEANUP_SPELLS(this_data)
            this_data = CLEANUP_LINES(this_data)

            for line in FIND_MONS_LINES.findall(this_data):
                monster_lines.extend(parse_mons_line(line))

            done.append(fname)

    if cull:
        return cull_unnamed_monsters(monster_lines)

    return monster_lines

def publish_monsters_as_cpp (monster_list, output):
    """
    Publish a list of monster specifications in a format that can be parsed by
    Gretell.

    :``monster_list``: The list of monster specifications to publish.
    :``output``: The file to write the output to. Must be an open, writable file
                 object.
    """
    mons_list_name = "vault_monsters"

    output.write("/**\n * @file vault_monster_data.cc\n * @author Jude Brown <bookofjude@users.sourceforge.net>\n * @version 1\n *\n * @section DESCRIPTION\n *\n * This file is automatically generated. Any changes to it will be discarded.\n *\n**/\n")
    output.write("#include \"AppHdr.h\"\n\n")
    output.write("/**\n * Return a vector of vault-defined monster specification strings.\n *\n * @return A vector of std::strings.\n *\n**/\n")
    output.write("std::vector<std::string> get_vault_monsters ()\n")
    output.write("{\n")
    output.write("    std::vector<std::string> %s;\n" % mons_list_name)
    output.write("    %s.reserve(%s);\n" % (mons_list_name, len(monster_list)));

    for mons in monster_list:
        output.write('    %s.push_back("%s");\n' % (mons_list_name, mons.replace('"', "'")))

    output.write("    return %s;\n" % mons_list_name)
    output.write("}\n")

def main (args):
    """
    Main entry-point.

    :``args``: A copy of sys.argv.
    """
    des_folder = DEFAULT_DES_FOLDER.replace("/", DIR_DELIM)
    output = DEFAULT_OUTPUT
    verbose = False

    if "-h" in args or "--help" in args:
        print main.__doc__ % (DEFAULT_DES_FOLDER, DEFAULT_OUTPUT)
        return

    if "-v" in args:
        verbose = True
        args.pop(args.index("-v"))
    elif "--verbose" in args:
        verbose = True
        args.pop(args.index("--verbose"))

    if args[0] == "python":
        del args[0]
    if args[0] == "parse_des.py":
        del args[0]

    if len(args) >= 1:
        des_folder = args.pop(0)

    if len(args) >= 1:
        output = args.pop(0)

    if not os.path.isdir(des_folder):
        raise MapParseError, "Specified des folder '%s' is not a folder!" % des_folder

    monsters = generate_monster_lines(des_folder, cull=True, verbose=verbose)
    output = open(output, "w")
    publish_monsters_as_cpp(monsters, output=output)
    output.close()

main.__doc__ = __doc__.lstrip()

if __name__=="__main__":
    main(sys.argv)
