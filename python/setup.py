from setuptools import setup, find_namespace_packages

REQUIRED = [
    "protobuf",
]
EXTRAS = {}

print(find_namespace_packages(exclude=["tests", "*.tests", "*.tests.*", "tests.*"]))

setup(name='zetasql',
    version='0.1',
    description='GRPC and Protobuf libraries for ZetaSQL',
    url='http://github.com/gbeanvamp/zetasql',
    author='Gregory Bean',
    author_email='gbean@videoamp.com',
    packages=find_namespace_packages(exclude=["tests", "*.tests", "*.tests.*", "tests.*"]),
    package_data = {
        'zetasql': ['py.typed', "**/*.pyi"],
    },
    zip_safe=False,
    install_requires=REQUIRED,
    extras_require=EXTRAS,
    include_package_data=True,
)
