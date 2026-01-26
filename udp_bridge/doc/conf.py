# conf.py for udp_bridge documentation

# -- Project information -----------------------------------------------------
project = 'udp_bridge'
copyright = '2024, Roland Arsenault'
author = 'Roland Arsenault'

# -- General configuration ---------------------------------------------------
extensions = [
    'sphinx.ext.autodoc',
    'sphinx.ext.intersphinx',
    'sphinx.ext.todo',
    'sphinx.ext.viewcode',
    'breathe',
    'exhale',
    'myst_parser',
]

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']

# -- Options for HTML output -------------------------------------------------
html_theme = 'sphinx_rtd_theme'
html_static_path = ['_static']

# -- Breathe/Exhale Configuration --------------------------------------------
# Setup the exhale extension
exhale_args = {
    # These arguments are required
    "containmentFolder":     "./generated",
    "rootFileName":          "library_root.rst",
    "doxygenStripFromPath":  "..",
    # Heavily encouraged optional argument (so you can identify it)
    "rootFileTitle":         "C++ API Reference",
    # Suggested optional arguments
    "createTreeView":        True,
    # TIP: if using the sphinx-bootstrap-theme, you need
    # "treeViewIsBootstrap": True,
    "exhaleExecutesDoxygen": False,
    # "exhaleDoxygenStdin":    "INPUT = ../include"
}

# Tell sphinx what the primary language being documented is.
primary_domain = 'cpp'

# Tell transition from rosdoc2 that we want to use the ReadTheDocs theme, not the default
# (optional, but good for consistency)
