import os
Import("env")

# Use ccache to speed up rebuilds
if os.system("ccache --version > nul 2>&1") == 0:
    env["CC"] = "ccache " + env["CC"]
    env["CXX"] = "ccache " + env["CXX"]
    print("ccache enabled for faster builds")
else:
    print("ccache not found - install it for faster rebuilds")
