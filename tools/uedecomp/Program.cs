// Decompile every UClass in an (uncompressed, see tools/upk_decompress.py) UE3 package to <out>/<Class>.uc.
//   dotnet run -c Release -- dumps/upk/C1Game.xxx dumps/script/C1Game [ClassFilter]
using UELib;
using UELib.Core;

var path = args[0];
var outDir = args[1];
var filter = args.Length > 2 ? args[2] : null;
Directory.CreateDirectory(outDir);
var package = UnrealLoader.LoadPackage(path, FileAccess.Read);
package.InitializePackage();
int ok = 0, failed = 0;
foreach (var obj in package.Objects)
{
    if (obj is not UClass cls || obj.ExportTable == null) continue;
    if (filter != null && !cls.Name.ToString().Contains(filter)) continue;
    try
    {
        File.WriteAllText(Path.Combine(outDir, cls.Name + ".uc"), cls.Decompile());
        ok++;
    }
    catch (Exception e)
    {
        File.WriteAllText(Path.Combine(outDir, cls.Name + ".uc.err"), e.ToString());
        failed++;
    }
}
Console.WriteLine($"{path}: {ok} classes decompiled, {failed} failed -> {outDir}");
