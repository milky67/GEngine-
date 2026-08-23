using System;
using System.IO;
using System.Text;
using Microsoft.Build.Construction;
using GodotTools.Shared;

namespace GodotTools.ProjectEditor
{
    public static class ProjectGenerator
    {
        public static string GodotSdkAttrValue => $"Godot.NET.Sdk/{GeneratedGodotNupkgsVersions.GodotNETSdk}";

        public static string GodotMinimumRequiredTfm => "net8.0";

        public static ProjectRootElement GenGameProject(string name)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("Project name is empty.", nameof(name));

            var root = ProjectRootElement.Create(NewProjectFileOptions.None);

            root.Sdk = GodotSdkAttrValue;

            var mainGroup = root.AddPropertyGroup();
            mainGroup.AddProperty("TargetFramework", GodotMinimumRequiredTfm);

            var net9 = mainGroup.AddProperty("TargetFramework", "net9.0");
            net9.Condition = " '$(GodotTargetPlatform)' == 'android' ";

            mainGroup.AddProperty("EnableDynamicLoading", "true");

            string sanitizedName = IdentifierUtils.SanitizeQualifiedIdentifier(name, allowEmptyIdentifiers: true);

            if (sanitizedName != name)
                mainGroup.AddProperty("RootNamespace", sanitizedName);

            return root;
        }

        public static string GenAndSaveGameProject(string dir, string name)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("Project name is empty.", nameof(name));

            string path = Path.Combine(dir, name + ".csproj");

            try
            {
                var root = GenGameProject(name);
                root.Save(path, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            }
            catch (Exception)
            {
                // Fallback direct XML string writer kapag nag-fail ang MSBuild construction sa Android
                var sb = new StringBuilder();
                sb.AppendLine($"<Project Sdk=\"{GodotSdkAttrValue}\">");
                sb.AppendLine("  <PropertyGroup>");
                sb.AppendLine($"    <TargetFramework>{GodotMinimumRequiredTfm}</TargetFramework>");
                sb.AppendLine("    <TargetFramework Condition=\" '$(GodotTargetPlatform)' == 'android' \">net9.0</TargetFramework>");
                sb.AppendLine("    <EnableDynamicLoading>true</EnableDynamicLoading>");
                sb.AppendLine("  </PropertyGroup>");
                sb.AppendLine("</Project>");

                File.WriteAllText(path, sb.ToString(), new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            }

            return Guid.NewGuid().ToString().ToUpperInvariant();
        }
    }
}
