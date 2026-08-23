using System;
using System.Globalization;
using System.IO;
using System.Text;
using GodotTools.Shared;

namespace GodotTools.ProjectEditor
{
    public static class ProjectGenerator
    {
        public static string GodotSdkAttrValue => $"Godot.NET.Sdk/{GeneratedGodotNupkgsVersions.GodotNETSdk}";

        public static string GodotMinimumRequiredTfm => "net8.0";

        public static string GenGameProjectXml(string name)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("Project name is empty.", nameof(name));

            string sanitizedName = IdentifierUtils.SanitizeQualifiedIdentifier(name, allowEmptyIdentifiers: true);

            var sb = new StringBuilder();
            sb.AppendLine($"<Project Sdk=\"{GodotSdkAttrValue}\">");
            sb.AppendLine("  <PropertyGroup>");
            sb.AppendLine($"    <TargetFramework>{GodotMinimumRequiredTfm}</TargetFramework>");
            sb.AppendLine("    <TargetFramework Condition=\" '$(GodotTargetPlatform)' == 'android' \">net9.0</TargetFramework>");
            sb.AppendLine("    <EnableDynamicLoading>true</EnableDynamicLoading>");

            if (sanitizedName != name)
            {
                sb.AppendLine($"    <RootNamespace>{sanitizedName}</RootNamespace>");
            }

            sb.AppendLine("  </PropertyGroup>");
            sb.AppendLine("</Project>");

            return sb.ToString();
        }

        public static string GenAndSaveGameProject(string dir, string name)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("Project name is empty.", nameof(name));

            string path = Path.Combine(dir, name + ".csproj");

            string xmlContent = GenGameProjectXml(name);

            // Save without BOM directly – safe on Android without MSBuild assembly reflection
            File.WriteAllText(path, xmlContent, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

            return Guid.NewGuid().ToString().ToUpperInvariant();
        }
    }
}
