import json
import os

# === CONFIGURATION ===
projectRoot = os.path.abspath('.')  # Absolute path to the project root
srcDir = os.path.join(projectRoot, 'src')  # Source files directory
buildDir = os.path.join(projectRoot, 'build')  # Build output directory
includeDirs = ['include']  # List of include directories
compiler = 'clang++'  # C++ compiler to use
flags = [
    '-std=c++20',  # Use C++20 standard
    '-Wall',       # Enable all common warnings
    '-Wextra',     # Enable extra warnings
    '-Iinclude'    # Add include directory
]

def getCppFiles(directory):
    """
    Recursively collect all .cpp files from the given directory.
    Returns a list of file paths.
    """
    cppFiles = []
    for root, _, files in os.walk(directory):
        for file in files:
            if file.endswith('.cpp'):
                fullPath = os.path.join(root, file)
                cppFiles.append(fullPath)
    return cppFiles

def generateCompileCommand(filePath):
    """
    Generate a compile_commands.json entry for a given source file.
    """
    relPath = os.path.relpath(filePath, projectRoot)  # Relative path to the file
    return {
        'directory': projectRoot,
        'command': f"{compiler} {' '.join(flags)} -c {relPath} -o /dev/null",
        'file': relPath
    }

def main():
    """
    Main entry point:
    - Finds all .cpp files in srcDir
    - Generates compile_commands.json for clangd/IDE integration
    """
    cppFiles = getCppFiles(srcDir)
    commands = [generateCompileCommand(f) for f in cppFiles]

    with open('compile_commands.json', 'w') as f:
        json.dump(commands, f, indent=4)  # Save in pretty JSON format

    print('[+] compile_commands.json generated.')

if __name__ == '__main__':
    main()