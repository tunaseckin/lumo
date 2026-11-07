#!/usr/bin/env python3
"""Package this folder as lumo-lang.vsix (no npm needed).
Install with:  code --install-extension lumo-lang.vsix   (or: cursor --install-extension ...)"""
import json, os, zipfile

here = os.path.dirname(os.path.abspath(__file__))
pkg = json.load(open(os.path.join(here, "package.json")))
files = ["package.json", "extension.js", "language-configuration.json", "README.md", "syntaxes/lumo.tmLanguage.json"]

manifest = f"""<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id="{pkg['name']}" Version="{pkg['version']}" Publisher="{pkg['publisher']}" />
    <DisplayName>{pkg['displayName']}</DisplayName>
    <Description xml:space="preserve">{pkg['description']}</Description>
    <Categories>Programming Languages</Categories>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="{pkg['engines']['vscode']}" />
    </Properties>
  </Metadata>
  <Installation><InstallationTarget Id="Microsoft.VisualStudio.Code" /></Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
"""
types = """<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension=".json" ContentType="application/json" />
  <Default Extension=".js" ContentType="application/javascript" />
  <Default Extension=".md" ContentType="text/markdown" />
  <Default Extension=".vsixmanifest" ContentType="text/xml" />
</Types>
"""
out = os.path.join(here, f"{pkg['name']}.vsix")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("extension.vsixmanifest", manifest)
    z.writestr("[Content_Types].xml", types)
    for f in files:
        z.write(os.path.join(here, f), "extension/" + f)
print(out)
