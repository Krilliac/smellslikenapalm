// Read-only, bounded metadata extraction for cooked RS2 UE3 map packages.
// Built by extract_cooked_map_metadata.ps1 with the installed Visual Studio C# compiler.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

internal sealed class UsageException : Exception
{
    public UsageException(string message) : base(message) { }
}

internal sealed class ValidationException : Exception
{
    public ValidationException(string message) : base(message) { }
}

internal sealed class PackageException : Exception
{
    public PackageException(string message, Exception inner) : base(message, inner) { }
}

internal sealed class Options
{
    public string InputPath;
    public string UELibPath;
    public string OutputPath;
    public string Mode = "actors";
    public string ClassPattern =
        "ROPlayerStart.*|ROSpawn.*|ROObjective.*|RO(?!Seq).*Capture.*|RO(?!Seq).*Territory.*|" +
        "ROPlaceableVolume.*|ROVolumePlayerStartGroup.*|ROVolumeSpawnProtection.*|" +
        "ROVolumeMapBounds.*|ROVolumeMapBoundary.*|WorldInfo|ROTeamInfo.*";
    public string PropertyPattern =
        "Location|Rotation|.*Team.*|.*Objective.*|Obj.*|.*Capture.*|.*Volume.*|" +
        ".*Spawn.*|DisplayName|Name|Tag|Group|Letter|.*Index.*|.*Priority.*|" +
        ".*Radius.*|.*Extent.*|.*Bounds.*|Initial.*|.*Active.*|.*Enabled.*|" +
        ".*Countdown.*|MinimumCapture.*|Order|.*Owner.*|.*Attack.*|.*Defend.*";
    public long MaxInputBytes = 512L * 1024L * 1024L;
    public int MaxExports = 250000;
    public int MaxActors = 1000;
    public int MaxProperties = 64;
    public int MaxValueChars = 1024;
    public int MaxErrors = 25;
    public int MaxClasses = 512;
    public long ObjectBase = -1;
    public string ExpectedPackageGuid;
    public string ExpectedSha256;
    public string VerifiedSha256;
    public bool Overwrite;
    public bool ClassPatternSpecified;
}

internal sealed class RoleExportPair
{
    public string RoleClass;
    public int UClassLinkerIndex;
    public int CdoLinkerIndex;
}

internal sealed class ExtractedProperty
{
    public string Name;
    public string Type;
    public int ArrayIndex;
    public int Size;
    public string Value;
}

internal sealed class VectorValue
{
    public double X;
    public double Y;
    public double Z;
}

internal sealed class ModelBoundsValue
{
    public int NetIndex;
    public VectorValue Origin;
    public VectorValue Extent;
    public double SphereRadius;
}

internal sealed class RawPropertyTag
{
    public string Name;
    public string Type;
    public int Size;
    public int ArrayIndex;
    public int ValueOffset;
}

internal sealed class RawRoleCount
{
    public string Team;
    public string SourceProperty;
    public int Ordinal;
    public int RoleInfoClassPackageIndex;
    public string RoleInfoClassPath;
    public byte Count;
    public byte ReverseCount;
    public int SerializedBytes;
}

internal enum RawRoleFieldKind
{
    RoleInfoClass,
    Count,
    ReverseCount
}

internal static class Program
{
    private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;
    private static readonly TimeSpan RegexTimeout = TimeSpan.FromSeconds(1);
    private const double MaxBoundMagnitude = 100000000.0;

    private const int ExitUsage = 64;
    private const int ExitValidation = 65;
    private const int ExitPackage = 66;
    private const int ExitPartial = 67;
    private const int ExitInternal = 70;
    private const string UnsafeAssemblySimpleName =
        "System.Runtime.CompilerServices.Unsafe";
    private const string UnsafeAssemblyFullName =
        "System.Runtime.CompilerServices.Unsafe, Version=6.0.3.0, " +
        "Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a";

    public static int Main(string[] args)
    {
        if (args != null && args.Length == 1 &&
            string.Equals(args[0], "--self-test-role-schema", StringComparison.Ordinal))
        {
            return RunRoleSchemaSelfTest();
        }
        if (args != null && args.Length == 1 &&
            string.Equals(args[0], "--self-test-role-export-schema", StringComparison.Ordinal))
        {
            return RunRoleExportSchemaSelfTest();
        }
        if (HasHelp(args))
        {
            PrintUsage(Console.Out);
            return 0;
        }

        try
        {
            Options options = ParseOptions(args);
            ValidateOptions(options);
            return Extract(options);
        }
        catch (UsageException ex)
        {
            WriteError("usage", ex.Message);
            PrintUsage(Console.Error);
            return ExitUsage;
        }
        catch (ValidationException ex)
        {
            WriteError("validation", ex.Message);
            return ExitValidation;
        }
        catch (PackageException ex)
        {
            WriteError("package", ex.Message + ": " + ExceptionMessage(ex.InnerException));
            return ExitPackage;
        }
        catch (Exception ex)
        {
            WriteError("internal", ExceptionMessage(ex));
            return ExitInternal;
        }
    }

    private static int Extract(Options options)
    {
        if (string.Equals(options.Mode, "role-exports", StringComparison.Ordinal))
        {
            options.VerifiedSha256 = ComputeSha256(options.InputPath);
            if (!string.Equals(
                    options.VerifiedSha256, options.ExpectedSha256,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new ValidationException(
                    "role-exports SHA-256 mismatch: expected " +
                    options.ExpectedSha256 + " but found " + options.VerifiedSha256);
            }
        }

        Regex classFilter = CreateRegex(options.ClassPattern, "class");
        Regex propertyFilter = CreateRegex(options.PropertyPattern, "property");
        string libraryDirectory = Path.GetDirectoryName(options.UELibPath);
        string unsafePath = Path.GetFullPath(Path.Combine(
            libraryDirectory, UnsafeAssemblySimpleName + ".dll"));
        if (!File.Exists(unsafePath))
        {
            throw new ValidationException(
                "pinned UELib dependency does not exist: " + unsafePath);
        }
        AssemblyName unsafeIdentity;
        Assembly pinnedUnsafe;
        try
        {
            unsafeIdentity = AssemblyName.GetAssemblyName(unsafePath);
            if (!string.Equals(
                    unsafeIdentity.FullName, UnsafeAssemblyFullName,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "expected " + UnsafeAssemblyFullName + " but found " +
                    unsafeIdentity.FullName);
            }
            pinnedUnsafe = Assembly.LoadFrom(unsafePath);
            if (!string.Equals(
                    pinnedUnsafe.GetName().FullName, UnsafeAssemblyFullName,
                    StringComparison.Ordinal) ||
                !string.Equals(
                    Path.GetFullPath(pinnedUnsafe.Location), unsafePath,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "the CLR did not load the exact pinned UELib dependency path");
            }
        }
        catch (Exception ex)
        {
            throw new ValidationException(
                "could not load the exact pinned UELib dependency: " +
                ExceptionMessage(ex));
        }
        ResolveEventHandler resolver = delegate(object sender, ResolveEventArgs eventArgs)
        {
            AssemblyName requested;
            try
            {
                requested = new AssemblyName(eventArgs.Name);
            }
            catch
            {
                return null;
            }
            return string.Equals(
                requested.FullName, UnsafeAssemblyFullName,
                StringComparison.Ordinal) ? pinnedUnsafe : null;
        };

        AppDomain.CurrentDomain.AssemblyResolve += resolver;
        object package = null;
        TextWriter output = null;
        bool ownsOutput = false;

        try
        {
            Assembly library;
            Type packageType;
            try
            {
                library = Assembly.LoadFrom(options.UELibPath);
                int unsafeReferences = 0;
                foreach (AssemblyName reference in library.GetReferencedAssemblies())
                {
                    if (!string.Equals(
                            reference.Name, UnsafeAssemblySimpleName,
                            StringComparison.Ordinal))
                    {
                        continue;
                    }
                    unsafeReferences++;
                    if (!string.Equals(
                            reference.FullName, UnsafeAssemblyFullName,
                            StringComparison.Ordinal) ||
                        !AssemblyName.ReferenceMatchesDefinition(
                            reference, unsafeIdentity))
                    {
                        throw new InvalidDataException(
                            "UELib dependency reference identity drifted: " +
                            reference.FullName);
                    }
                }
                if (unsafeReferences != 1)
                {
                    throw new InvalidDataException(
                        "UELib must reference the pinned Unsafe dependency exactly once");
                }
                packageType = library.GetType("UELib.UnrealPackage", true);
                MethodInfo deserialize = packageType.GetMethod(
                    "DeserializePackage",
                    BindingFlags.Public | BindingFlags.Static,
                    null,
                    new[] { typeof(string), typeof(FileAccess) },
                    null);
                if (deserialize == null)
                {
                    throw new MissingMethodException(
                        "UELib.UnrealPackage.DeserializePackage(string, FileAccess)");
                }

                package = Quiet(delegate
                {
                    return deserialize.Invoke(
                        null, new object[] { options.InputPath, FileAccess.Read });
                });
                if (package == null)
                {
                    throw new InvalidDataException("UELib returned a null package");
                }
            }
            catch (Exception ex)
            {
                throw new PackageException("could not deserialize the package tables", Unwrap(ex));
            }

            dynamic packageView = package;
            int nameCount = CollectionCount(packageView.Names, "name table");
            int importCount = CollectionCount(packageView.Imports, "import table");
            int exportCount = CollectionCount(packageView.Exports, "export table");
            bool roleExportsMode = string.Equals(
                options.Mode, "role-exports", StringComparison.Ordinal);
            long packageFlags = 0;
            int generationCount = 0;
            long finalGenerationExports = 0;
            long finalGenerationNames = 0;
            long finalGenerationNetObjects = 0;
            if (roleExportsMode)
            {
                packageFlags = Convert.ToInt64(packageView.PackageFlags, Invariant);
                generationCount = CollectionCount(
                    packageView.Generations, "generation table");
                foreach (object rawGeneration in (IEnumerable)packageView.Generations)
                {
                    dynamic generation = rawGeneration;
                    finalGenerationExports = Convert.ToInt64(
                        generation.ExportsCount, Invariant);
                    finalGenerationNames = Convert.ToInt64(
                        generation.NamesCount, Invariant);
                    finalGenerationNetObjects = Convert.ToInt64(
                        generation.NetObjectsCount, Invariant);
                }
            }
            if (exportCount > options.MaxExports)
            {
                throw new ValidationException(
                    "package export count " + exportCount.ToString(Invariant) +
                    " exceeds --max-exports " + options.MaxExports.ToString(Invariant));
            }

            string packageGuid = SafeString(packageView.GUID);
            string packageName = SafeString(packageView.PackageName);
            bool isMap = Quiet(delegate { return (bool)packageView.IsMap(); });
            bool isCooked = Quiet(delegate { return (bool)packageView.IsCooked(); });
            if (roleExportsMode)
            {
                ValidateRoleExportPackage(
                    options, packageName, packageGuid, isMap, packageFlags,
                    generationCount, exportCount, finalGenerationNetObjects);
                string recheckedSha256 = ComputeSha256(options.InputPath);
                if (!string.Equals(
                        recheckedSha256, options.VerifiedSha256,
                        StringComparison.Ordinal))
                {
                    throw new ValidationException(
                        "role-exports input changed while package tables were read");
                }
            }

            if (options.OutputPath == null)
            {
                output = new StreamWriter(
                    Console.OpenStandardOutput(), new UTF8Encoding(false), 65536, true);
            }
            else
            {
                string parent = Path.GetDirectoryName(options.OutputPath);
                if (!string.IsNullOrEmpty(parent))
                {
                    Directory.CreateDirectory(parent);
                }
                output = new StreamWriter(
                    new FileStream(options.OutputPath, FileMode.CreateNew, FileAccess.Write, FileShare.Read),
                    new UTF8Encoding(false), 65536);
                ownsOutput = true;
            }

            FileVersionInfo libraryVersion = FileVersionInfo.GetVersionInfo(options.UELibPath);
            EmitHeader(
                output,
                options,
                library.GetName().Version == null ? null : library.GetName().Version.ToString(),
                libraryVersion.ProductVersion,
                packageName,
                packageGuid,
                Convert.ToInt64(packageView.Version, Invariant),
                Convert.ToInt64(packageView.LicenseeVersion, Invariant),
                Convert.ToInt64(packageView.EngineVersion, Invariant),
                isMap,
                isCooked,
                nameCount,
                importCount,
                exportCount,
                packageFlags,
                generationCount,
                finalGenerationExports,
                finalGenerationNames,
                finalGenerationNetObjects);

            int result;
            if (string.Equals(options.Mode, "classes", StringComparison.Ordinal))
            {
                result = ExtractClasses(output, packageView, options, classFilter);
            }
            else if (string.Equals(options.Mode, "role-exports", StringComparison.Ordinal))
            {
                result = ExtractRoleExports(
                    output, packageView, options, exportCount,
                    finalGenerationNetObjects);
            }
            else if (string.Equals(options.Mode, "brush-bounds", StringComparison.Ordinal))
            {
                result = ExtractBrushBounds(
                    output, library, packageType, packageView, options, classFilter, nameCount);
            }
            else if (string.Equals(options.Mode, "role-info", StringComparison.Ordinal))
            {
                result = ExtractRoleInfo(output, packageView, options, nameCount);
            }
            else
            {
                result = ExtractActors(
                    output, library, packageType, packageView, options, classFilter, propertyFilter);
            }

            output.Flush();
            return result;
        }
        finally
        {
            if (package is IDisposable)
            {
                ((IDisposable)package).Dispose();
            }
            if (output != null)
            {
                output.Flush();
                if (ownsOutput)
                {
                    output.Dispose();
                }
            }
            AppDomain.CurrentDomain.AssemblyResolve -= resolver;
        }
    }

    private static int ExtractClasses(
        TextWriter output, dynamic package, Options options, Regex classFilter)
    {
        SortedDictionary<string, int> counts =
            new SortedDictionary<string, int>(StringComparer.Ordinal);
        foreach (object rawExport in (IEnumerable)package.Exports)
        {
            dynamic export = rawExport;
            string className = SafeString(export.ClassName);
            if (className.Length == 0 || !IsMatch(classFilter, className, "class"))
            {
                continue;
            }

            int current;
            counts.TryGetValue(className, out current);
            counts[className] = current + 1;
        }

        int emitted = 0;
        foreach (KeyValuePair<string, int> entry in counts)
        {
            if (emitted >= options.MaxClasses)
            {
                break;
            }
            EmitClass(output, entry.Key, entry.Value);
            emitted++;
        }

        EmitClassSummary(output, counts.Count, emitted, counts.Count > emitted);
        return 0;
    }

    private static void ValidateRoleExportPackage(
        Options options, string packageName, string packageGuid, bool isMap,
        long packageFlags, int generationCount, int exportCount,
        long finalGenerationNetObjects)
    {
        if (!string.Equals(packageName, "ROGame", StringComparison.Ordinal) ||
            !string.Equals(
                Path.GetFileNameWithoutExtension(options.InputPath), "ROGame",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new ValidationException(
                "role-exports requires the root ROGame.u package");
        }
        if (isMap)
        {
            throw new ValidationException(
                "role-exports requires the non-map root ROGame.u package");
        }
        // UELib 1.12.1 reports IsCooked=false for the pinned retail ROGame.u,
        // despite its exact 0x20204001 cooked-script flags. Validate the source
        // table invariants directly instead of trusting that helper.
        if ((packageFlags & 0x00200000L) == 0 || generationCount <= 0 ||
            finalGenerationNetObjects != exportCount)
        {
            throw new ValidationException(
                "role-exports requires a script package whose final NetObjectCount " +
                "matches its export table");
        }
        if (!IsExactHex(packageGuid, 32) || IsAllZeroHex(packageGuid))
        {
            throw new ValidationException(
                "role-exports requires a valid nonzero 32-digit package GUID");
        }
        if (!string.Equals(
                packageGuid, options.ExpectedPackageGuid,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new ValidationException(
                "role-exports package GUID mismatch: expected " +
                options.ExpectedPackageGuid + " but found " + packageGuid);
        }
    }

    private static int ExtractRoleExports(
        TextWriter output, dynamic package, Options options,
        int exportCount, long finalGenerationNetObjects)
    {
        Dictionary<string, object> uclasses =
            new Dictionary<string, object>(StringComparer.Ordinal);
        Dictionary<string, object> cdos =
            new Dictionary<string, object>(StringComparer.Ordinal);
        HashSet<int> matchedIndices = new HashSet<int>();

        int exportOrdinal = 0;
        foreach (object rawExport in (IEnumerable)package.Exports)
        {
            dynamic export = rawExport;
            string objectName = SafeString(export.ObjectName);
            string className = SafeString(export.ClassName);
            string roleClass = null;
            bool isUClass =
                string.Equals(className, "Class", StringComparison.Ordinal) &&
                IsRoleClassName(objectName);
            bool isCdo = objectName.StartsWith(
                    "Default__", StringComparison.Ordinal) &&
                IsRoleClassName(className) &&
                string.Equals(
                    objectName, "Default__" + className,
                    StringComparison.Ordinal);
            if (!isUClass && !isCdo)
            {
                exportOrdinal++;
                continue;
            }

            roleClass = isUClass ? objectName : className;
            int linkerIndex = Convert.ToInt32(export.Index, Invariant);
            if (linkerIndex <= 0 || linkerIndex != exportOrdinal ||
                linkerIndex >= exportCount ||
                linkerIndex >= finalGenerationNetObjects ||
                !matchedIndices.Add(linkerIndex))
            {
                throw new PackageException(
                    "role-exports found an invalid, out-of-range, or duplicate linker index",
                    new InvalidDataException(
                        roleClass + " index=" + linkerIndex.ToString(Invariant) +
                        " ordinal=" + exportOrdinal.ToString(Invariant)));
            }
            if (export.OuterTable != null)
            {
                throw new PackageException(
                    "role-exports found a nested role object",
                    new InvalidDataException(roleClass));
            }

            Dictionary<string, object> target = isUClass ? uclasses : cdos;
            AddUniqueRoleExport(target, roleClass, rawExport);
            exportOrdinal++;
        }
        if (exportOrdinal != exportCount)
        {
            throw new PackageException(
                "role-exports did not consume the exact export table",
                new InvalidDataException(
                    "enumerated=" + exportOrdinal.ToString(Invariant) +
                    " expected=" + exportCount.ToString(Invariant)));
        }

        ValidateRolePairNameSets(uclasses, cdos);

        List<RoleExportPair> pairs = new List<RoleExportPair>();
        foreach (KeyValuePair<string, object> entry in uclasses)
        {
            object rawCdo;
            if (!cdos.TryGetValue(entry.Key, out rawCdo))
            {
                throw new PackageException(
                    "role-exports found a UClass without its exact CDO",
                    new InvalidDataException(entry.Key));
            }

            dynamic uclass = entry.Value;
            dynamic cdo = rawCdo;
            int uclassIndex = Convert.ToInt32(uclass.Index, Invariant);
            int cdoIndex = Convert.ToInt32(cdo.Index, Invariant);
            object rawClassTable = cdo.ClassTable;
            if (rawClassTable == null)
            {
                throw new PackageException(
                    "role-exports found a CDO without an in-package class table",
                    new InvalidDataException(entry.Key));
            }
            dynamic classTable = rawClassTable;
            int cdoClassIndex = Convert.ToInt32(classTable.Index, Invariant);
            dynamic uclassPackageIndex = uclass.ClassIndex;
            if (!(bool)uclassPackageIndex.IsNull || uclass.ClassTable != null)
            {
                throw new PackageException(
                    "role-exports UClass does not use the exact UELib Class identity",
                    new InvalidDataException(entry.Key));
            }
            dynamic cdoClassPackageIndex = cdo.ClassIndex;
            ValidateRolePairIndices(
                entry.Key, uclassIndex, cdoIndex, cdoClassIndex,
                Convert.ToInt32(cdoClassPackageIndex.Index, Invariant),
                (bool)cdoClassPackageIndex.IsExport);
            if (!object.ReferenceEquals(rawClassTable, entry.Value))
            {
                throw new PackageException(
                    "role-exports CDO does not reference its exact UClass",
                    new InvalidDataException(
                        entry.Key + " expected=" + uclassIndex.ToString(Invariant) +
                        " actual=" + cdoClassIndex.ToString(Invariant)));
            }
            pairs.Add(new RoleExportPair
            {
                RoleClass = entry.Key,
                UClassLinkerIndex = uclassIndex,
                CdoLinkerIndex = cdoIndex
            });
        }

        pairs.Sort(delegate(RoleExportPair left, RoleExportPair right)
        {
            return left.UClassLinkerIndex.CompareTo(right.UClassLinkerIndex);
        });
        foreach (RoleExportPair pair in pairs)
        {
            long? uclassStaticReference = null;
            long? cdoStaticReference = null;
            if (options.ObjectBase >= 0)
            {
                uclassStaticReference = CheckedStaticReference(
                    options.ObjectBase, pair.UClassLinkerIndex);
                cdoStaticReference = CheckedStaticReference(
                    options.ObjectBase, pair.CdoLinkerIndex);
            }
            EmitRoleExport(
                output, pair, options.ObjectBase,
                uclassStaticReference, cdoStaticReference);
        }
        EmitRoleExportSummary(
            output, pairs.Count, options.ObjectBase >= 0);
        return 0;
    }

    private static void AddUniqueRoleExport(
        Dictionary<string, object> target, string roleClass, object export)
    {
        if (target.ContainsKey(roleClass))
        {
            throw new PackageException(
                "role-exports found a duplicate role object",
                new InvalidDataException(roleClass));
        }
        target.Add(roleClass, export);
    }

    private static void ValidateRolePairNameSets(
        Dictionary<string, object> uclasses,
        Dictionary<string, object> cdos)
    {
        if (uclasses.Count == 0 || uclasses.Count != cdos.Count)
        {
            throw new PackageException(
                "role-exports found an incomplete UClass/CDO role table",
                new InvalidDataException(
                    "uclasses=" + uclasses.Count.ToString(Invariant) +
                    " cdos=" + cdos.Count.ToString(Invariant)));
        }
        foreach (string roleClass in uclasses.Keys)
        {
            if (!cdos.ContainsKey(roleClass))
            {
                throw new PackageException(
                    "role-exports found a UClass without its exact CDO",
                    new InvalidDataException(roleClass));
            }
        }
        foreach (string roleClass in cdos.Keys)
        {
            if (!uclasses.ContainsKey(roleClass))
            {
                throw new PackageException(
                    "role-exports found a CDO without its exact UClass",
                    new InvalidDataException(roleClass));
            }
        }
    }

    private static void ValidateRolePairIndices(
        string roleClass, int uclassIndex, int cdoIndex,
        int cdoClassTableIndex, int cdoClassPackageIndex,
        bool cdoClassPackageIsExport)
    {
        if (uclassIndex <= 0 || cdoIndex <= 0 ||
            cdoIndex != uclassIndex + 1 ||
            cdoClassTableIndex != uclassIndex ||
            !cdoClassPackageIsExport ||
            cdoClassPackageIndex != uclassIndex + 1)
        {
            throw new PackageException(
                "role-exports CDO does not reference its exact UClass",
                new InvalidDataException(
                    roleClass + " uclass=" + uclassIndex.ToString(Invariant) +
                    " cdo=" + cdoIndex.ToString(Invariant) +
                    " classTable=" + cdoClassTableIndex.ToString(Invariant) +
                    " classPackage=" + cdoClassPackageIndex.ToString(Invariant)));
        }
    }

    private static bool IsRoleClassName(string value)
    {
        const string prefix = "RORoleInfo";
        if (string.IsNullOrEmpty(value) ||
            !value.StartsWith(prefix, StringComparison.Ordinal))
        {
            return false;
        }
        for (int index = prefix.Length; index < value.Length; index++)
        {
            char character = value[index];
            if (!((character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') ||
                  character == '_'))
            {
                return false;
            }
        }
        return true;
    }

    private static long CheckedStaticReference(long objectBase, int linkerIndex)
    {
        long result = checked(objectBase + linkerIndex);
        if (objectBase < 0 || linkerIndex <= 0 || result >= 0x80000000L)
        {
            throw new ValidationException(
                "role-exports ObjectBase + linker index must remain below 0x80000000");
        }
        return result;
    }

    // UELib 1.12.1 cannot infer the custom element type of
    // ROMapInfo.NorthernRoles/SouthernRoles and renders each array as
    // "Array type was not detected."  The installed RS2 source defines the
    // element as native RORoleCount { Class<RORoleInfo>, byte, byte }.  UE3
    // serializes each array element as a tagged struct, so decode that exact
    // source-defined layout directly from the ROMapInfo export and require
    // complete byte consumption.  This mode does not infer runtime squads:
    // InitSquadsForGametype clears and constructs those after map load.
    private static int ExtractRoleInfo(
        TextWriter output, dynamic package, Options options, int nameCount)
    {
        string[] names = BuildNameTable(package, nameCount);

        object mapInfoExport = null;
        int mapInfoExports = 0;
        foreach (object rawExport in (IEnumerable)package.Exports)
        {
            dynamic export = rawExport;
            if (!string.Equals(
                    SafeString(export.ClassName), "ROMapInfo",
                    StringComparison.Ordinal))
            {
                continue;
            }
            mapInfoExport = rawExport;
            mapInfoExports++;
        }
        if (mapInfoExports != 1 || mapInfoExport == null)
        {
            throw new PackageException(
                "role-info requires exactly one ROMapInfo export",
                new InvalidDataException(
                    "found " + mapInfoExports.ToString(Invariant)));
        }

        dynamic mapInfo = mapInfoExport;
        long serialOffset = Convert.ToInt64(mapInfo.SerialOffset, Invariant);
        long serialSize = Convert.ToInt64(mapInfo.SerialSize, Invariant);
        const int maximumRoleInfoSerialBytes = 1024 * 1024;
        if (serialOffset < 0 || serialSize < 12 ||
            serialSize > maximumRoleInfoSerialBytes || serialSize > int.MaxValue)
        {
            throw new PackageException(
                "ROMapInfo export has an invalid bounded serial range",
                new InvalidDataException(
                    "offset=" + serialOffset.ToString(Invariant) +
                    " size=" + serialSize.ToString(Invariant)));
        }

        byte[] data = new byte[(int)serialSize];
        using (FileStream input = new FileStream(
            options.InputPath, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            if (serialOffset > input.Length - serialSize)
            {
                throw new PackageException(
                    "ROMapInfo export extends outside the package",
                    new InvalidDataException(serialOffset.ToString(Invariant)));
            }
            input.Seek(serialOffset, SeekOrigin.Begin);
            ReadExactly(input, data, "ROMapInfo export");
        }

        Dictionary<string, RawPropertyTag> wanted =
            new Dictionary<string, RawPropertyTag>(StringComparer.Ordinal);
        int cursor = 4; // UObject::Serialize writes NetIndex first.
        while (cursor < data.Length)
        {
            RawPropertyTag tag = ReadRawPropertyTag(data, ref cursor, names, data.Length);
            if (tag == null)
            {
                break;
            }
            if (string.Equals(tag.Name, "NorthernRoles", StringComparison.Ordinal) ||
                string.Equals(tag.Name, "SouthernRoles", StringComparison.Ordinal))
            {
                if (wanted.ContainsKey(tag.Name))
                {
                    throw new PackageException(
                        "ROMapInfo contains a duplicate role array",
                        new InvalidDataException(tag.Name));
                }
                if (!string.Equals(tag.Type, "ArrayProperty", StringComparison.Ordinal) ||
                    tag.ArrayIndex != 0)
                {
                    throw new PackageException(
                        "ROMapInfo role field is not an exact scalar ArrayProperty",
                        new InvalidDataException(
                            tag.Name + ": " + tag.Type + "[" +
                            tag.ArrayIndex.ToString(Invariant) + "]"));
                }
                wanted.Add(tag.Name, tag);
            }
            cursor = CheckedAdvance(tag.ValueOffset, tag.Size, data.Length, tag.Name);
        }

        if (!wanted.ContainsKey("NorthernRoles") ||
            !wanted.ContainsKey("SouthernRoles"))
        {
            throw new PackageException(
                "ROMapInfo is missing a serialized role array",
                new InvalidDataException(
                    "NorthernRoles=" + wanted.ContainsKey("NorthernRoles") +
                    " SouthernRoles=" + wanted.ContainsKey("SouthernRoles")));
        }

        int emitted = 0;
        emitted += DecodeAndEmitRoleArray(
            output, package, data, names, wanted["NorthernRoles"], "north");
        emitted += DecodeAndEmitRoleArray(
            output, package, data, names, wanted["SouthernRoles"], "south");
        EmitRoleInfoSummary(
            output,
            Convert.ToInt32(mapInfo.Index, Invariant),
            Convert.ToInt64(mapInfo.SerialOffset, Invariant),
            Convert.ToInt64(mapInfo.SerialSize, Invariant),
            emitted);
        return 0;
    }

    private static string[] BuildNameTable(dynamic package, int nameCount)
    {
        string[] names = new string[nameCount];
        bool[] populated = new bool[nameCount];
        foreach (object rawName in (IEnumerable)package.Names)
        {
            dynamic name = rawName;
            int index = Convert.ToInt32(name.Index, Invariant);
            if (index < 0 || index >= nameCount || populated[index])
            {
                throw new PackageException(
                    "name table contains an invalid or duplicate index",
                    new InvalidDataException(index.ToString(Invariant)));
            }
            names[index] = SafeString(name.Name);
            populated[index] = true;
        }
        for (int index = 0; index < names.Length; index++)
        {
            if (!populated[index])
            {
                throw new PackageException(
                    "name table has a missing index",
                    new InvalidDataException(index.ToString(Invariant)));
            }
        }
        return names;
    }

    private static RawPropertyTag ReadRawPropertyTag(
        byte[] data, ref int cursor, string[] names, int limit)
    {
        string name = ReadRawName(data, ref cursor, names, limit, "property name");
        if (string.Equals(name, "None", StringComparison.Ordinal))
        {
            return null;
        }
        string type = ReadRawName(data, ref cursor, names, limit, name + " type");
        int size = ReadRawInt32(data, ref cursor, limit, name + " size");
        int arrayIndex = ReadRawInt32(data, ref cursor, limit, name + " array index");
        if (size < 0 || arrayIndex < 0)
        {
            throw new PackageException(
                "property tag contains a negative size or array index",
                new InvalidDataException(name));
        }

        if (string.Equals(type, "StructProperty", StringComparison.Ordinal) ||
            string.Equals(type, "ByteProperty", StringComparison.Ordinal))
        {
            ReadRawName(data, ref cursor, names, limit, name + " type metadata");
        }
        else if (string.Equals(type, "BoolProperty", StringComparison.Ordinal))
        {
            CheckedAdvance(cursor, 1, limit, name + " bool tag");
            cursor++;
        }

        return new RawPropertyTag
        {
            Name = name,
            Type = type,
            Size = size,
            ArrayIndex = arrayIndex,
            ValueOffset = cursor
        };
    }

    private static int DecodeAndEmitRoleArray(
        TextWriter output,
        dynamic package,
        byte[] data,
        string[] names,
        RawPropertyTag arrayTag,
        string team)
    {
        int end = CheckedAdvance(
            arrayTag.ValueOffset, arrayTag.Size, data.Length, arrayTag.Name);
        int cursor = arrayTag.ValueOffset;
        int count = ReadRawInt32(data, ref cursor, end, arrayTag.Name + " count");
        if (count < 1 || count > 64)
        {
            throw new PackageException(
                "ROMapInfo role count is outside the bounded range",
                new InvalidDataException(
                    arrayTag.Name + "=" + count.ToString(Invariant)));
        }

        for (int ordinal = 0; ordinal < count; ordinal++)
        {
            int elementStart = cursor;
            bool sawClass = false;
            bool sawCount = false;
            bool sawReverseCount = false;
            int classPackageIndex = 0;
            string classPath = null;
            byte roleCount = 0;
            byte reverseCount = 0;

            while (cursor < end)
            {
                RawPropertyTag field = ReadRawPropertyTag(data, ref cursor, names, end);
                if (field == null)
                {
                    break;
                }
                int fieldEnd = CheckedAdvance(
                    field.ValueOffset, field.Size, end,
                    arrayTag.Name + "[" + ordinal.ToString(Invariant) + "]." + field.Name);

                RawRoleFieldKind fieldKind = ClassifyRoleField(field, arrayTag.Name);
                if (fieldKind == RawRoleFieldKind.RoleInfoClass)
                {
                    if (sawClass)
                    {
                        throw new PackageException(
                            "duplicate RORoleCount.RoleInfoClass field",
                            new InvalidDataException(arrayTag.Name));
                    }
                    int valueCursor = field.ValueOffset;
                    classPackageIndex = ReadRawInt32(
                        data, ref valueCursor, fieldEnd, "RoleInfoClass value");
                    if (classPackageIndex == 0)
                    {
                        throw new PackageException(
                            "RORoleCount contains a null RoleInfoClass",
                            new InvalidDataException(arrayTag.Name));
                    }
                    object table = package.GetIndexTable(classPackageIndex);
                    if (table == null)
                    {
                        throw new PackageException(
                            "RORoleCount RoleInfoClass reference does not resolve",
                            new InvalidDataException(classPackageIndex.ToString(Invariant)));
                    }
                    dynamic tableView = table;
                    classPath = SafeString(tableView.GetReferencePath());
                    if (!classPath.StartsWith("Class'ROGame.RORoleInfo", StringComparison.Ordinal) ||
                        !classPath.EndsWith("'", StringComparison.Ordinal))
                    {
                        throw new PackageException(
                            "RORoleCount RoleInfoClass resolves outside ROGame role classes",
                            new InvalidDataException(classPath));
                    }
                    sawClass = true;
                }
                else if (fieldKind == RawRoleFieldKind.Count)
                {
                    if (sawCount)
                    {
                        throw new PackageException(
                            "duplicate RORoleCount.Count field",
                            new InvalidDataException(arrayTag.Name));
                    }
                    roleCount = data[field.ValueOffset];
                    sawCount = true;
                }
                else
                {
                    if (sawReverseCount)
                    {
                        throw new PackageException(
                            "duplicate RORoleCount.ReverseCount field",
                            new InvalidDataException(arrayTag.Name));
                    }
                    reverseCount = data[field.ValueOffset];
                    sawReverseCount = true;
                }
                cursor = fieldEnd;
            }

            if (!sawClass || !sawCount || !sawReverseCount)
            {
                throw new PackageException(
                    "RORoleCount element is missing a required field",
                    new InvalidDataException(
                        arrayTag.Name + "[" + ordinal.ToString(Invariant) + "]"));
            }

            EmitRoleCount(output, new RawRoleCount
            {
                Team = team,
                SourceProperty = arrayTag.Name,
                Ordinal = ordinal,
                RoleInfoClassPackageIndex = classPackageIndex,
                RoleInfoClassPath = classPath,
                Count = roleCount,
                ReverseCount = reverseCount,
                SerializedBytes = cursor - elementStart
            });
        }

        if (cursor != end)
        {
            throw new PackageException(
                "ROMapInfo role array was not consumed exactly",
                new InvalidDataException(
                    arrayTag.Name + " consumed=" +
                    (cursor - arrayTag.ValueOffset).ToString(Invariant) +
                    " expected=" + arrayTag.Size.ToString(Invariant)));
        }
        return count;
    }

    private static RawRoleFieldKind ClassifyRoleField(
        RawPropertyTag field, string arrayName)
    {
        RawRoleFieldKind kind;
        string expectedType;
        int expectedSize;
        if (string.Equals(field.Name, "RoleInfoClass", StringComparison.Ordinal))
        {
            kind = RawRoleFieldKind.RoleInfoClass;
            expectedType = "ObjectProperty";
            expectedSize = 4;
        }
        else if (string.Equals(field.Name, "Count", StringComparison.Ordinal))
        {
            kind = RawRoleFieldKind.Count;
            expectedType = "ByteProperty";
            expectedSize = 1;
        }
        else if (string.Equals(field.Name, "ReverseCount", StringComparison.Ordinal))
        {
            kind = RawRoleFieldKind.ReverseCount;
            expectedType = "ByteProperty";
            expectedSize = 1;
        }
        else
        {
            throw new PackageException(
                "RORoleCount contains an unexpected field",
                new InvalidDataException(arrayName + "." + field.Name));
        }

        if (field.ArrayIndex != 0 || field.Size != expectedSize ||
            !string.Equals(field.Type, expectedType, StringComparison.Ordinal))
        {
            throw new PackageException(
                "invalid exact RORoleCount." + field.Name + " field",
                new InvalidDataException(
                    arrayName + ": " + field.Type + "[" +
                    field.ArrayIndex.ToString(Invariant) + "] size=" +
                    field.Size.ToString(Invariant)));
        }
        return kind;
    }

    private static int RunRoleSchemaSelfTest()
    {
        RawPropertyTag[] valid =
        {
            new RawPropertyTag
            {
                Name = "RoleInfoClass", Type = "ObjectProperty", Size = 4,
                ArrayIndex = 0
            },
            new RawPropertyTag
            {
                Name = "Count", Type = "ByteProperty", Size = 1,
                ArrayIndex = 0
            },
            new RawPropertyTag
            {
                Name = "ReverseCount", Type = "ByteProperty", Size = 1,
                ArrayIndex = 0
            }
        };
        for (int index = 0; index < valid.Length; index++)
        {
            ClassifyRoleField(valid[index], "SyntheticRoles");
        }

        RawPropertyTag unknown = new RawPropertyTag
        {
            Name = "Unexpected", Type = "ByteProperty", Size = 1,
            ArrayIndex = 0
        };
        if (!RoleSchemaRejects(unknown))
        {
            WriteError("self-test", "unexpected RORoleCount field was accepted");
            return ExitInternal;
        }

        for (int index = 0; index < valid.Length; index++)
        {
            RawPropertyTag nonScalar = new RawPropertyTag
            {
                Name = valid[index].Name,
                Type = valid[index].Type,
                Size = valid[index].Size,
                ArrayIndex = 1
            };
            if (!RoleSchemaRejects(nonScalar))
            {
                WriteError(
                    "self-test",
                    "nonzero ArrayIndex was accepted for " + nonScalar.Name);
                return ExitInternal;
            }
        }

        Console.Out.WriteLine(
            "PASS: exact role schema rejects unknown fields and nonzero scalar ArrayIndex values.");
        return 0;
    }

    private static bool RoleSchemaRejects(RawPropertyTag field)
    {
        try
        {
            ClassifyRoleField(field, "SyntheticRoles");
            return false;
        }
        catch (PackageException)
        {
            return true;
        }
    }

    private static int RunRoleExportSchemaSelfTest()
    {
        string[] accepted =
        {
            "RORoleInfo",
            "RORoleInfoNorthernGuerilla",
            "RORoleInfoSouthernMachineGunner_SK"
        };
        string[] rejected =
        {
            null,
            string.Empty,
            "Default__RORoleInfoNorthernGuerilla",
            "RORoleInfo/NorthernGuerilla",
            "OtherRoleInfo"
        };
        foreach (string value in accepted)
        {
            if (!IsRoleClassName(value))
            {
                WriteError("self-test", "valid role class was rejected: " + value);
                return ExitInternal;
            }
        }
        foreach (string value in rejected)
        {
            if (IsRoleClassName(value))
            {
                WriteError("self-test", "invalid role class was accepted: " + value);
                return ExitInternal;
            }
        }
        if (CheckedStaticReference(39478, 47921) != 87399)
        {
            WriteError("self-test", "static-reference derivation drifted");
            return ExitInternal;
        }
        bool overflowRejected = false;
        try
        {
            CheckedStaticReference(0x7FFFFFF0L, 32);
        }
        catch (ValidationException)
        {
            overflowRejected = true;
        }
        if (!overflowRejected)
        {
            WriteError("self-test", "out-of-range static reference was accepted");
            return ExitInternal;
        }
        ValidateRolePairIndices(
            "RORoleInfoNorthernGuerilla", 47921, 47922,
            47921, 47922, true);
        if (!RolePairRejects(47921, 47922, 47920, 47922, true) ||
            !RolePairRejects(47921, 47923, 47921, 47922, true) ||
            !RolePairRejects(47921, 47922, 47921, 47921, true) ||
            !RolePairRejects(47921, 47922, 47921, 47922, false))
        {
            WriteError("self-test", "mismatched UClass/CDO pair was accepted");
            return ExitInternal;
        }
        object syntheticUClass = new object();
        object syntheticCdo = new object();
        Dictionary<string, object> syntheticUClasses =
            new Dictionary<string, object>(StringComparer.Ordinal);
        Dictionary<string, object> syntheticCdos =
            new Dictionary<string, object>(StringComparer.Ordinal);
        AddUniqueRoleExport(
            syntheticUClasses, "RORoleInfoSynthetic", syntheticUClass);
        AddUniqueRoleExport(
            syntheticCdos, "RORoleInfoSynthetic", syntheticCdo);
        ValidateRolePairNameSets(syntheticUClasses, syntheticCdos);
        bool missingPairRejected = false;
        try
        {
            ValidateRolePairNameSets(
                syntheticUClasses,
                new Dictionary<string, object>(StringComparer.Ordinal));
        }
        catch (PackageException)
        {
            missingPairRejected = true;
        }
        bool duplicateRejected = false;
        try
        {
            AddUniqueRoleExport(
                syntheticUClasses, "RORoleInfoSynthetic", new object());
        }
        catch (PackageException)
        {
            duplicateRejected = true;
        }
        if (!missingPairRejected || !duplicateRejected)
        {
            WriteError("self-test", "missing or duplicate role pair was accepted");
            return ExitInternal;
        }

        Console.Out.WriteLine(
            "PASS: role-export schema pins names, UClass/CDO pairs, and checked static references.");
        return 0;
    }

    private static bool RolePairRejects(
        int uclassIndex, int cdoIndex, int classTableIndex,
        int classPackageIndex, bool classPackageIsExport)
    {
        try
        {
            ValidateRolePairIndices(
                "SyntheticRole", uclassIndex, cdoIndex,
                classTableIndex, classPackageIndex, classPackageIsExport);
            return false;
        }
        catch (PackageException)
        {
            return true;
        }
    }

    private static bool IsExactHex(string value, int digits)
    {
        if (value == null || value.Length != digits)
        {
            return false;
        }
        for (int index = 0; index < value.Length; index++)
        {
            char character = value[index];
            if (!((character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'F') ||
                  (character >= 'a' && character <= 'f')))
            {
                return false;
            }
        }
        return true;
    }

    private static bool IsAllZeroHex(string value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return false;
        }
        for (int index = 0; index < value.Length; index++)
        {
            if (value[index] != '0')
            {
                return false;
            }
        }
        return true;
    }

    private static string ComputeSha256(string path)
    {
        using (FileStream input = new FileStream(
            path, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024,
            FileOptions.SequentialScan))
        using (SHA256 hash = SHA256.Create())
        {
            byte[] digest = hash.ComputeHash(input);
            StringBuilder rendered = new StringBuilder(digest.Length * 2);
            foreach (byte value in digest)
            {
                rendered.Append(value.ToString("X2", Invariant));
            }
            return rendered.ToString();
        }
    }

    private static string ReadRawName(
        byte[] data, ref int cursor, string[] names, int limit, string label)
    {
        int index = ReadRawInt32(data, ref cursor, limit, label + " index");
        int instance = ReadRawInt32(data, ref cursor, limit, label + " instance");
        if (index < 0 || index >= names.Length || instance != 0)
        {
            throw new PackageException(
                "invalid FName in " + label,
                new InvalidDataException(
                    "index=" + index.ToString(Invariant) +
                    " instance=" + instance.ToString(Invariant)));
        }
        return names[index];
    }

    private static int ReadRawInt32(
        byte[] data, ref int cursor, int limit, string label)
    {
        CheckedAdvance(cursor, 4, limit, label);
        int value = ReadInt32LittleEndian(data, cursor);
        cursor += 4;
        return value;
    }

    private static int CheckedAdvance(int offset, int count, int limit, string label)
    {
        if (offset < 0 || count < 0 || offset > limit - count)
        {
            throw new PackageException(
                "truncated raw metadata while reading " + label,
                new EndOfStreamException(
                    "offset=" + offset.ToString(Invariant) +
                    " count=" + count.ToString(Invariant) +
                    " limit=" + limit.ToString(Invariant)));
        }
        return offset + count;
    }

    private static void ReadExactly(FileStream input, byte[] data, string label)
    {
        int read = 0;
        while (read < data.Length)
        {
            int current = input.Read(data, read, data.Length - read);
            if (current <= 0)
            {
                throw new PackageException(
                    label + " ended early",
                    new EndOfStreamException(
                        read.ToString(Invariant) + "/" +
                        data.Length.ToString(Invariant)));
            }
            read += current;
        }
    }

    private static int ExtractActors(
        TextWriter output,
        Assembly library,
        Type packageType,
        dynamic package,
        Options options,
        Regex classFilter,
        Regex propertyFilter)
    {
        Type initFlagsType = library.GetType("UELib.UnrealPackage+InitFlags", true);
        object allFlags = Enum.Parse(initFlagsType, "All");
        MethodInfo initialize = packageType.GetMethod(
            "InitializePackage",
            BindingFlags.Public | BindingFlags.Instance,
            null,
            new[] { initFlagsType },
            null);
        if (initialize == null)
        {
            throw new PackageException(
                "could not initialize package objects",
                new MissingMethodException("UELib.UnrealPackage.InitializePackage(InitFlags)"));
        }

        try
        {
            Quiet(delegate { initialize.Invoke(package, new[] { allFlags }); });
        }
        catch (Exception ex)
        {
            throw new PackageException("could not initialize package objects", Unwrap(ex));
        }

        int candidates = 0;
        int emitted = 0;
        int errors = 0;
        int propertiesTruncated = 0;
        bool stoppedOnErrors = false;

        foreach (object rawObject in (IEnumerable)package.Objects)
        {
            dynamic actor = rawObject;
            string actorPath = null;
            string className = null;
            try
            {
                object exportTable = actor.ExportTable;
                if (exportTable == null)
                {
                    continue;
                }

                string actorName = SafeString(actor.Name);
                if (actorName.StartsWith("Default__", StringComparison.Ordinal))
                {
                    continue;
                }

                className = SafeString(actor.GetClassName());
                if (!IsMatch(classFilter, className, "class"))
                {
                    continue;
                }

                actorPath = SafeString(actor.GetPath());
                if (actorPath.IndexOf(".TheWorld.", StringComparison.Ordinal) < 0)
                {
                    continue;
                }

                candidates++;
                if (emitted >= options.MaxActors)
                {
                    continue;
                }

                Quiet(delegate { actor.Load(); });
                Exception objectError = actor.ThrownException as Exception;
                if (objectError != null)
                {
                    throw objectError;
                }

                List<ExtractedProperty> properties = new List<ExtractedProperty>();
                int matchingProperties = 0;
                double locationX = 0;
                double locationY = 0;
                double locationZ = 0;
                bool hasLocation = false;

                object propertyCollection = actor.Properties;
                if (propertyCollection != null)
                {
                    foreach (object rawProperty in (IEnumerable)propertyCollection)
                    {
                        dynamic property = rawProperty;
                        string propertyName = SafeString(property.Name);
                        if (!IsMatch(propertyFilter, propertyName, "property"))
                        {
                            continue;
                        }

                        matchingProperties++;
                        if (properties.Count >= options.MaxProperties)
                        {
                            continue;
                        }

                        string value = SafeString(property.Value);
                        if (value.Length > options.MaxValueChars)
                        {
                            value = value.Substring(0, options.MaxValueChars);
                        }
                        properties.Add(new ExtractedProperty
                        {
                            Name = propertyName,
                            Type = SafeString(property.Type),
                            ArrayIndex = Convert.ToInt32(property.ArrayIndex, Invariant),
                            Size = Convert.ToInt32(property.Size, Invariant),
                            Value = value
                        });

                        if (!hasLocation &&
                            string.Equals(propertyName, "Location", StringComparison.OrdinalIgnoreCase))
                        {
                            hasLocation = TryParseVector(value, out locationX, out locationY, out locationZ);
                        }
                    }
                }

                bool truncated = matchingProperties > properties.Count;
                if (truncated)
                {
                    propertiesTruncated++;
                }

                dynamic export = actor.ExportTable;
                EmitActor(
                    output,
                    emitted,
                    className,
                    GetClassPath(export, className),
                    SafeString(actor.Name),
                    actorPath,
                    Convert.ToInt32(export.Index, Invariant),
                    Convert.ToInt64(export.SerialOffset, Invariant),
                    Convert.ToInt64(export.SerialSize, Invariant),
                    matchingProperties,
                    truncated,
                    hasLocation,
                    locationX,
                    locationY,
                    locationZ,
                    properties);
                emitted++;
            }
            catch (RegexMatchTimeoutException ex)
            {
                throw new ValidationException("regex timed out while matching " + ex.Input);
            }
            catch (Exception ex)
            {
                errors++;
                EmitActorError(output, actorPath, className, ExceptionMessage(Unwrap(ex)));
                if (errors >= options.MaxErrors)
                {
                    stoppedOnErrors = true;
                    break;
                }
            }
        }

        EmitActorSummary(
            output,
            candidates,
            emitted,
            candidates > emitted,
            propertiesTruncated,
            errors,
            stoppedOnErrors);
        return errors == 0 ? 0 : ExitPartial;
    }

    private static bool IsBrushTransformProperty(string propertyName)
    {
        return string.Equals(propertyName, "Rotation", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(propertyName, "DrawScale", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(propertyName, "DrawScale3D", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(propertyName, "PrePivot", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(propertyName, "MainScale", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(propertyName, "PostScale", StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryReadModelBounds(
        FileStream input,
        dynamic export,
        int noneNameIndex,
        out ModelBoundsValue bounds,
        out string error)
    {
        bounds = null;
        error = null;

        const int netIndexBytes = 4;
        const int nameBytes = 8;
        const int boundsBytes = 7 * 4;
        const int requiredBytes = netIndexBytes + nameBytes + boundsBytes;

        long serialOffset = Convert.ToInt64(export.SerialOffset, Invariant);
        long serialSize = Convert.ToInt64(export.SerialSize, Invariant);
        if (serialOffset < 0 || serialSize < requiredBytes ||
            serialOffset > input.Length - requiredBytes)
        {
            error = "UModel export is too small or outside the package";
            return false;
        }

        byte[] data = new byte[requiredBytes];
        input.Seek(serialOffset, SeekOrigin.Begin);
        int read = 0;
        while (read < data.Length)
        {
            int current = input.Read(data, read, data.Length - read);
            if (current <= 0)
            {
                error = "UModel export ended before its native bounds";
                return false;
            }
            read += current;
        }

        int netIndex = ReadInt32LittleEndian(data, 0);
        int tagNameIndex = ReadInt32LittleEndian(data, netIndexBytes);
        int tagNameInstance = ReadInt32LittleEndian(data, netIndexBytes + 4);
        if (tagNameIndex != noneNameIndex || tagNameInstance != 0)
        {
            error =
                "UModel does not have the validated NetIndex + FName(None) prefix; " +
                "native bounds offset is unknown";
            return false;
        }

        int nativeOffset = netIndexBytes + nameBytes;
        VectorValue origin = new VectorValue
        {
            X = ReadSingleLittleEndian(data, nativeOffset),
            Y = ReadSingleLittleEndian(data, nativeOffset + 4),
            Z = ReadSingleLittleEndian(data, nativeOffset + 8)
        };
        VectorValue extent = new VectorValue
        {
            X = ReadSingleLittleEndian(data, nativeOffset + 12),
            Y = ReadSingleLittleEndian(data, nativeOffset + 16),
            Z = ReadSingleLittleEndian(data, nativeOffset + 20)
        };
        double radius = ReadSingleLittleEndian(data, nativeOffset + 24);

        if (!IsBoundedVector(origin) || !IsBoundedVector(extent) ||
            !IsFinite(radius) || Math.Abs(radius) > MaxBoundMagnitude)
        {
            error = "UModel FBoxSphereBounds contains non-finite or out-of-cap values";
            return false;
        }
        if (extent.X < 0 || extent.Y < 0 || extent.Z < 0 || radius < 0)
        {
            error = "UModel FBoxSphereBounds contains a negative extent or radius";
            return false;
        }

        double maximumExtent = Math.Max(extent.X, Math.Max(extent.Y, extent.Z));
        double diagonal = Math.Sqrt(
            extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z);
        double tolerance = Math.Max(0.25, diagonal * 0.00001);
        if (radius + tolerance < maximumExtent || radius > diagonal + tolerance)
        {
            error = "UModel sphere radius is inconsistent with its box extent";
            return false;
        }

        bounds = new ModelBoundsValue
        {
            NetIndex = netIndex,
            Origin = origin,
            Extent = extent,
            SphereRadius = radius
        };
        return true;
    }

    private static int ReadInt32LittleEndian(byte[] data, int offset)
    {
        uint bits = (uint)data[offset] |
            ((uint)data[offset + 1] << 8) |
            ((uint)data[offset + 2] << 16) |
            ((uint)data[offset + 3] << 24);
        return unchecked((int)bits);
    }

    private static float ReadSingleLittleEndian(byte[] data, int offset)
    {
        int bits = ReadInt32LittleEndian(data, offset);
        return BitConverter.ToSingle(BitConverter.GetBytes(bits), 0);
    }

    private static bool IsBoundedVector(VectorValue value)
    {
        return value != null &&
            IsFinite(value.X) && IsFinite(value.Y) && IsFinite(value.Z) &&
            Math.Abs(value.X) <= MaxBoundMagnitude &&
            Math.Abs(value.Y) <= MaxBoundMagnitude &&
            Math.Abs(value.Z) <= MaxBoundMagnitude;
    }

    private static bool IsFinite(double value)
    {
        return !double.IsNaN(value) && !double.IsInfinity(value);
    }

    private static int ExtractBrushBounds(
        TextWriter output,
        Assembly library,
        Type packageType,
        dynamic package,
        Options options,
        Regex classFilter,
        int nameCount)
    {
        Type initFlagsType = library.GetType("UELib.UnrealPackage+InitFlags", true);
        object allFlags = Enum.Parse(initFlagsType, "All");
        MethodInfo initialize = packageType.GetMethod(
            "InitializePackage",
            BindingFlags.Public | BindingFlags.Instance,
            null,
            new[] { initFlagsType },
            null);
        if (initialize == null)
        {
            throw new PackageException(
                "could not initialize package objects",
                new MissingMethodException("UELib.UnrealPackage.InitializePackage(InitFlags)"));
        }

        try
        {
            Quiet(delegate { initialize.Invoke(package, new[] { allFlags }); });
        }
        catch (Exception ex)
        {
            throw new PackageException("could not initialize package objects", Unwrap(ex));
        }

        int noneNameIndex = -1;
        foreach (object rawName in (IEnumerable)package.Names)
        {
            dynamic name = rawName;
            int index = Convert.ToInt32(name.Index, Invariant);
            if (index < 0 || index >= nameCount)
            {
                throw new PackageException(
                    "name table contains an out-of-range index",
                    new InvalidDataException(index.ToString(Invariant)));
            }
            if (string.Equals(SafeString(name.Name), "None", StringComparison.Ordinal))
            {
                if (noneNameIndex >= 0)
                {
                    throw new PackageException(
                        "name table contains duplicate None entries",
                        new InvalidDataException("ambiguous tagged-property terminator"));
                }
                noneNameIndex = index;
            }
        }
        if (noneNameIndex < 0)
        {
            throw new PackageException(
                "name table does not contain None",
                new InvalidDataException("missing tagged-property terminator"));
        }

        Dictionary<string, object> modelsByPath =
            new Dictionary<string, object>(StringComparer.Ordinal);
        HashSet<string> duplicateModelPaths = new HashSet<string>(StringComparer.Ordinal);
        foreach (object rawObject in (IEnumerable)package.Objects)
        {
            dynamic candidate = rawObject;
            object exportTable = candidate.ExportTable;
            if (exportTable == null ||
                !string.Equals(SafeString(candidate.GetClassName()), "Model", StringComparison.Ordinal))
            {
                continue;
            }

            string path = SafeString(candidate.GetPath());
            if (path.IndexOf(".TheWorld.", StringComparison.Ordinal) < 0)
            {
                continue;
            }
            if (modelsByPath.ContainsKey(path))
            {
                duplicateModelPaths.Add(path);
            }
            else
            {
                modelsByPath.Add(path, rawObject);
            }
        }

        int candidates = 0;
        int emitted = 0;
        int errors = 0;
        bool stoppedOnErrors = false;
        FileStream input = null;

        try
        {
            input = new FileStream(
                options.InputPath, FileMode.Open, FileAccess.Read, FileShare.Read,
                65536, FileOptions.RandomAccess);

            foreach (object rawObject in (IEnumerable)package.Objects)
            {
                dynamic actor = rawObject;
                string actorPath = null;
                string className = null;
                try
                {
                    object actorExportTable = actor.ExportTable;
                    if (actorExportTable == null)
                    {
                        continue;
                    }

                    className = SafeString(actor.GetClassName());
                    if (!IsMatch(classFilter, className, "class"))
                    {
                        continue;
                    }
                    actorPath = SafeString(actor.GetPath());
                    if (actorPath.IndexOf(".TheWorld.", StringComparison.Ordinal) < 0 ||
                        SafeString(actor.Name).StartsWith("Default__", StringComparison.Ordinal))
                    {
                        continue;
                    }

                    candidates++;
                    if (emitted >= options.MaxActors)
                    {
                        continue;
                    }

                    Quiet(delegate { actor.Load(); });
                    Exception objectError = actor.ThrownException as Exception;
                    if (objectError != null)
                    {
                        throw objectError;
                    }

                    bool hasLocation = false;
                    double locationX = 0;
                    double locationY = 0;
                    double locationZ = 0;
                    string modelPath = null;
                    List<string> unsupportedTransforms = new List<string>();

                    object propertyCollection = actor.Properties;
                    if (propertyCollection != null)
                    {
                        foreach (object rawProperty in (IEnumerable)propertyCollection)
                        {
                            dynamic property = rawProperty;
                            string propertyName = SafeString(property.Name);
                            if (string.Equals(propertyName, "Location", StringComparison.OrdinalIgnoreCase))
                            {
                                string value = Quiet(delegate { return SafeString(property.Value); });
                                hasLocation = TryParseVector(
                                    value, out locationX, out locationY, out locationZ);
                            }
                            else if (string.Equals(propertyName, "BRUSH", StringComparison.OrdinalIgnoreCase))
                            {
                                string value = Quiet(delegate { return SafeString(property.Value); });
                                Match match = Regex.Match(
                                    value,
                                    @"Model'(?<path>[^'\r\n]{1,2048})'",
                                    RegexOptions.CultureInvariant,
                                    RegexTimeout);
                                if (match.Success)
                                {
                                    modelPath = match.Groups["path"].Value;
                                }
                            }
                            else if (IsBrushTransformProperty(propertyName))
                            {
                                unsupportedTransforms.Add(propertyName);
                            }
                        }
                    }

                    if (!hasLocation)
                    {
                        throw new InvalidDataException(
                            "brush actor has no finite serialized Location");
                    }
                    if (unsupportedTransforms.Count != 0)
                    {
                        throw new InvalidDataException(
                            "non-translation brush transform is not supported: " +
                            string.Join(",", unsupportedTransforms.ToArray()));
                    }
                    if (string.IsNullOrEmpty(modelPath))
                    {
                        throw new InvalidDataException("BRUSH does not reference a UModel");
                    }
                    int lastSeparator = modelPath.LastIndexOf('.');
                    if (lastSeparator <= 0 ||
                        !string.Equals(
                            modelPath.Substring(0, lastSeparator), actorPath,
                            StringComparison.Ordinal))
                    {
                        throw new InvalidDataException(
                            "BRUSH UModel is not owned by the selected actor");
                    }
                    if (duplicateModelPaths.Contains(modelPath))
                    {
                        throw new InvalidDataException("duplicate UModel path: " + modelPath);
                    }

                    object rawModel;
                    if (!modelsByPath.TryGetValue(modelPath, out rawModel))
                    {
                        throw new InvalidDataException("referenced UModel export is missing");
                    }
                    dynamic model = rawModel;
                    dynamic modelExport = model.ExportTable;
                    ModelBoundsValue bounds;
                    string boundsError;
                    if (!TryReadModelBounds(
                        input, modelExport, noneNameIndex, out bounds, out boundsError))
                    {
                        throw new InvalidDataException(boundsError);
                    }

                    VectorValue location = new VectorValue
                    {
                        X = locationX,
                        Y = locationY,
                        Z = locationZ
                    };
                    VectorValue worldMin = new VectorValue
                    {
                        X = locationX + bounds.Origin.X - bounds.Extent.X,
                        Y = locationY + bounds.Origin.Y - bounds.Extent.Y,
                        Z = locationZ + bounds.Origin.Z - bounds.Extent.Z
                    };
                    VectorValue worldMax = new VectorValue
                    {
                        X = locationX + bounds.Origin.X + bounds.Extent.X,
                        Y = locationY + bounds.Origin.Y + bounds.Extent.Y,
                        Z = locationZ + bounds.Origin.Z + bounds.Extent.Z
                    };
                    if (!IsBoundedVector(worldMin) || !IsBoundedVector(worldMax))
                    {
                        throw new InvalidDataException(
                            "translated world AABB is non-finite or outside the numeric cap");
                    }

                    dynamic actorExport = actor.ExportTable;
                    EmitBrushBounds(
                        output,
                        emitted,
                        className,
                        actorPath,
                        Convert.ToInt32(actorExport.Index, Invariant),
                        Convert.ToInt64(actorExport.SerialOffset, Invariant),
                        Convert.ToInt64(actorExport.SerialSize, Invariant),
                        modelPath,
                        Convert.ToInt32(modelExport.Index, Invariant),
                        Convert.ToInt64(modelExport.SerialOffset, Invariant),
                        Convert.ToInt64(modelExport.SerialSize, Invariant),
                        bounds.NetIndex,
                        noneNameIndex,
                        location,
                        bounds,
                        worldMin,
                        worldMax);
                    emitted++;
                }
                catch (RegexMatchTimeoutException ex)
                {
                    throw new ValidationException("regex timed out while matching " + ex.Input);
                }
                catch (Exception ex)
                {
                    errors++;
                    EmitBrushBoundsError(
                        output, actorPath, className, ExceptionMessage(Unwrap(ex)));
                    if (errors >= options.MaxErrors)
                    {
                        stoppedOnErrors = true;
                        break;
                    }
                }
            }
        }
        finally
        {
            if (input != null)
            {
                input.Dispose();
            }
        }

        EmitBrushBoundsSummary(
            output,
            candidates,
            emitted,
            candidates > emitted,
            errors,
            stoppedOnErrors,
            modelsByPath.Count);
        return errors == 0 ? 0 : ExitPartial;
    }

    private static Options ParseOptions(string[] args)
    {
        if (args == null || args.Length == 0)
        {
            throw new UsageException("arguments are required");
        }

        Options options = new Options();
        for (int index = 0; index < args.Length; index++)
        {
            string argument = args[index];
            switch (argument)
            {
                case "--input": options.InputPath = NextValue(args, ref index, argument); break;
                case "--uelib": options.UELibPath = NextValue(args, ref index, argument); break;
                case "--output": options.OutputPath = NextValue(args, ref index, argument); break;
                case "--mode": options.Mode = NextValue(args, ref index, argument); break;
                case "--class-pattern":
                    options.ClassPattern = NextValue(args, ref index, argument);
                    options.ClassPatternSpecified = true;
                    break;
                case "--property-pattern": options.PropertyPattern = NextValue(args, ref index, argument); break;
                case "--max-input-bytes": options.MaxInputBytes = ParseLong(NextValue(args, ref index, argument), argument, 1, 8L * 1024L * 1024L * 1024L * 1024L); break;
                case "--max-exports": options.MaxExports = ParseInt(NextValue(args, ref index, argument), argument, 1, 2000000); break;
                case "--max-actors": options.MaxActors = ParseInt(NextValue(args, ref index, argument), argument, 1, 100000); break;
                case "--max-properties": options.MaxProperties = ParseInt(NextValue(args, ref index, argument), argument, 1, 4096); break;
                case "--max-value-chars": options.MaxValueChars = ParseInt(NextValue(args, ref index, argument), argument, 16, 65536); break;
                case "--max-errors": options.MaxErrors = ParseInt(NextValue(args, ref index, argument), argument, 1, 1000); break;
                case "--max-classes": options.MaxClasses = ParseInt(NextValue(args, ref index, argument), argument, 1, 10000); break;
                case "--object-base": options.ObjectBase = ParseLong(NextValue(args, ref index, argument), argument, 0, 0x7FFFFFFF); break;
                case "--expected-package-guid": options.ExpectedPackageGuid = NextValue(args, ref index, argument); break;
                case "--expected-sha256": options.ExpectedSha256 = NextValue(args, ref index, argument); break;
                case "--overwrite": options.Overwrite = true; break;
                default: throw new UsageException("unknown argument: " + argument);
            }
        }
        return options;
    }

    private static void ValidateOptions(Options options)
    {
        if (string.IsNullOrWhiteSpace(options.InputPath))
        {
            throw new ValidationException("--input is required");
        }
        if (string.IsNullOrWhiteSpace(options.UELibPath))
        {
            throw new ValidationException("--uelib is required");
        }
        if (!string.Equals(options.Mode, "actors", StringComparison.Ordinal) &&
            !string.Equals(options.Mode, "classes", StringComparison.Ordinal) &&
            !string.Equals(options.Mode, "role-info", StringComparison.Ordinal) &&
            !string.Equals(options.Mode, "role-exports", StringComparison.Ordinal) &&
            !string.Equals(options.Mode, "brush-bounds", StringComparison.Ordinal))
        {
            throw new ValidationException(
                "--mode must be actors, classes, role-info, role-exports, or brush-bounds");
        }
        bool roleExports = string.Equals(
            options.Mode, "role-exports", StringComparison.Ordinal);
        if (roleExports)
        {
            if (!IsExactHex(options.ExpectedPackageGuid, 32) ||
                IsAllZeroHex(options.ExpectedPackageGuid))
            {
                throw new ValidationException(
                    "role-exports requires --expected-package-guid with 32 nonzero hex digits");
            }
            if (!IsExactHex(options.ExpectedSha256, 64) ||
                IsAllZeroHex(options.ExpectedSha256))
            {
                throw new ValidationException(
                    "role-exports requires --expected-sha256 with 64 nonzero hex digits");
            }
            options.ExpectedPackageGuid = options.ExpectedPackageGuid.ToUpperInvariant();
            options.ExpectedSha256 = options.ExpectedSha256.ToUpperInvariant();
        }
        else if (options.ObjectBase >= 0 ||
                 options.ExpectedPackageGuid != null ||
                 options.ExpectedSha256 != null)
        {
            throw new ValidationException(
                "--object-base and expected package identity arguments require role-exports mode");
        }
        if (string.Equals(options.Mode, "brush-bounds", StringComparison.Ordinal) &&
            !options.ClassPatternSpecified)
        {
            options.ClassPattern = "BlockingVolume";
        }
        if (string.Equals(options.Mode, "brush-bounds", StringComparison.Ordinal) &&
            !string.Equals(options.ClassPattern, "BlockingVolume", StringComparison.OrdinalIgnoreCase))
        {
            throw new ValidationException(
                "brush-bounds currently supports only the exact BlockingVolume class");
        }

        options.InputPath = FullPath(options.InputPath, "input");
        options.UELibPath = FullPath(options.UELibPath, "UELib");
        string unsafeDependencyPath = Path.GetFullPath(Path.Combine(
            Path.GetDirectoryName(options.UELibPath),
            UnsafeAssemblySimpleName + ".dll"));
        if (options.OutputPath != null)
        {
            options.OutputPath = FullPath(options.OutputPath, "output");
        }

        if (!File.Exists(options.InputPath))
        {
            throw new ValidationException("input package does not exist: " + options.InputPath);
        }
        string expectedExtension = roleExports ? ".u" : ".roe";
        if (!string.Equals(
                Path.GetExtension(options.InputPath), expectedExtension,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new ValidationException(
                "input must be a " + expectedExtension + " package in " +
                options.Mode + " mode: " + options.InputPath);
        }
        if (!File.Exists(options.UELibPath))
        {
            throw new ValidationException("Eliot.UELib.dll does not exist: " + options.UELibPath);
        }

        long inputBytes = new FileInfo(options.InputPath).Length;
        if (inputBytes > options.MaxInputBytes)
        {
            throw new ValidationException(
                "input is " + inputBytes.ToString(Invariant) +
                " bytes, exceeding --max-input-bytes " + options.MaxInputBytes.ToString(Invariant));
        }

        if (options.OutputPath != null)
        {
            if (SamePath(options.OutputPath, options.InputPath) ||
                SamePath(options.OutputPath, options.UELibPath) ||
                SamePath(options.OutputPath, unsafeDependencyPath))
            {
                throw new ValidationException(
                    "output must not overwrite an input or UELib dependency file");
            }
            if (File.Exists(options.OutputPath))
            {
                if (!options.Overwrite)
                {
                    throw new ValidationException(
                        "output exists; pass --overwrite to replace it: " + options.OutputPath);
                }
                File.Delete(options.OutputPath);
            }
        }

        CreateRegex(options.ClassPattern, "class");
        CreateRegex(options.PropertyPattern, "property");
    }

    private static Regex CreateRegex(string pattern, string label)
    {
        if (string.IsNullOrWhiteSpace(pattern))
        {
            throw new ValidationException(label + " pattern must not be empty");
        }
        try
        {
            return new Regex(
                "^(?:" + pattern + ")$",
                RegexOptions.IgnoreCase | RegexOptions.CultureInvariant,
                RegexTimeout);
        }
        catch (ArgumentException ex)
        {
            throw new ValidationException("invalid " + label + " regex: " + ex.Message);
        }
    }

    private static bool IsMatch(Regex regex, string value, string label)
    {
        try
        {
            return regex.IsMatch(value ?? string.Empty);
        }
        catch (RegexMatchTimeoutException)
        {
            throw new ValidationException(label + " regex timed out");
        }
    }

    private static string GetClassPath(dynamic export, string fallback)
    {
        try
        {
            object table = export.ClassTable;
            if (table == null)
            {
                return fallback;
            }
            dynamic classTable = table;
            string path = SafeString(classTable.GetReferencePath());
            return path.Length == 0 ? fallback : path;
        }
        catch
        {
            return fallback;
        }
    }

    private static bool TryParseVector(
        string value, out double x, out double y, out double z)
    {
        x = 0;
        y = 0;
        z = 0;
        if (string.IsNullOrEmpty(value))
        {
            return false;
        }

        Match match = Regex.Match(
            value,
            @"^\(X=(?<x>[-+0-9.eE]+),Y=(?<y>[-+0-9.eE]+),Z=(?<z>[-+0-9.eE]+)\)$",
            RegexOptions.CultureInvariant,
            RegexTimeout);
        return match.Success &&
            double.TryParse(match.Groups["x"].Value, NumberStyles.Float, Invariant, out x) &&
            double.TryParse(match.Groups["y"].Value, NumberStyles.Float, Invariant, out y) &&
            double.TryParse(match.Groups["z"].Value, NumberStyles.Float, Invariant, out z);
    }

    private static void EmitHeader(
        TextWriter output,
        Options options,
        string assemblyVersion,
        string productVersion,
        string packageName,
        string packageGuid,
        long packageVersion,
        long licenseeVersion,
        long engineVersion,
        bool isMap,
        bool isCooked,
        int names,
        int imports,
        int exports,
        long packageFlags,
        int generationCount,
        long finalGenerationExports,
        long finalGenerationNames,
        long finalGenerationNetObjects)
    {
        StringBuilder json = BeginRecord("header");
        AddString(json, "input", options.InputPath);
        AddLong(json, "inputBytes", new FileInfo(options.InputPath).Length);
        if (options.VerifiedSha256 != null)
        {
            AddString(json, "inputSha256", options.VerifiedSha256);
            AddBool(json, "sha256MatchedExpectation", true);
            AddBool(json, "sha256StableAcrossTableRead", true);
        }
        AddString(json, "mode", options.Mode);
        AddString(json, "uelibAssemblyVersion", assemblyVersion);
        AddString(json, "uelibProductVersion", productVersion);
        AddString(json, "packageName", packageName);
        AddString(json, "packageGuid", packageGuid);
        AddLong(json, "packageVersion", packageVersion);
        AddLong(json, "licenseeVersion", licenseeVersion);
        AddLong(json, "engineVersion", engineVersion);
        AddBool(json, "isMap", isMap);
        AddBool(json, "isCooked", isCooked);
        AddLong(json, "names", names);
        AddLong(json, "imports", imports);
        AddLong(json, "exports", exports);
        if (string.Equals(options.Mode, "role-exports", StringComparison.Ordinal))
        {
            AddString(
                json, "packageFlags",
                "0x" + packageFlags.ToString("X8", Invariant));
            AddLong(json, "generationCount", generationCount);
            AddLong(json, "finalGenerationExports", finalGenerationExports);
            AddLong(json, "finalGenerationNames", finalGenerationNames);
            AddLong(json, "finalGenerationNetObjects", finalGenerationNetObjects);
        }
        AddString(json, "classPattern", options.ClassPattern);
        AddString(json, "propertyPattern", options.PropertyPattern);
        AddLong(json, "maxActors", options.MaxActors);
        AddLong(json, "maxProperties", options.MaxProperties);
        AddLong(json, "maxValueChars", options.MaxValueChars);
        EndRecord(output, json);
    }

    private static void EmitRoleExport(
        TextWriter output,
        RoleExportPair pair,
        long objectBase,
        long? uclassStaticReference,
        long? cdoStaticReference)
    {
        StringBuilder json = BeginRecord("roleExport");
        AddLong(json, "schemaVersion", 1);
        AddString(json, "roleClass", pair.RoleClass);
        AddLong(json, "uclassLinkerIndex", pair.UClassLinkerIndex);
        AddLong(json, "cdoLinkerIndex", pair.CdoLinkerIndex);
        if (objectBase >= 0)
        {
            AddLong(json, "objectBase", objectBase);
            AddLong(json, "uclassStaticReference", uclassStaticReference.Value);
            AddLong(json, "cdoStaticReference", cdoStaticReference.Value);
            AddString(
                json, "referenceDerivation",
                "PackageMap ObjectBase + linker export index");
        }
        AddBool(json, "derivedOnly", true);
        AddBool(json, "authorizedByExtractor", false);
        EndRecord(output, json);
    }

    private static void EmitRoleExportSummary(
        TextWriter output, int pairedRoles, bool objectBaseSupplied)
    {
        StringBuilder json = BeginRecord("summary");
        AddString(json, "mode", "role-exports");
        AddLong(json, "pairedRoles", pairedRoles);
        AddLong(json, "uclassExports", pairedRoles);
        AddLong(json, "cdoExports", pairedRoles);
        AddBool(json, "pairsValidatedExactly", true);
        AddBool(json, "tableOnly", true);
        AddBool(json, "objectBaseSupplied", objectBaseSupplied);
        AddBool(json, "runtimeRolesAuthorized", false);
        EndRecord(output, json);
    }

    private static void EmitRoleCount(TextWriter output, RawRoleCount role)
    {
        StringBuilder json = BeginRecord("role");
        AddLong(json, "schemaVersion", 1);
        AddString(json, "team", role.Team);
        AddString(json, "sourceProperty", role.SourceProperty);
        AddLong(json, "ordinal", role.Ordinal);
        AddLong(json, "roleInfoClassPackageIndex", role.RoleInfoClassPackageIndex);
        AddString(json, "roleInfoClassPath", role.RoleInfoClassPath);
        AddLong(json, "count", role.Count);
        AddLong(json, "reverseCount", role.ReverseCount);
        AddLong(json, "serializedBytes", role.SerializedBytes);
        AddString(json, "elementSchema", "ROMapInfo.RORoleCount tagged struct");
        EndRecord(output, json);
    }

    private static void EmitRoleInfoSummary(
        TextWriter output,
        int mapInfoExportIndex,
        long mapInfoSerialOffset,
        long mapInfoSerialSize,
        int emittedRoles)
    {
        StringBuilder json = BeginRecord("summary");
        AddString(json, "mode", "role-info");
        AddLong(json, "mapInfoExportIndex", mapInfoExportIndex);
        AddLong(json, "mapInfoSerialOffset", mapInfoSerialOffset);
        AddLong(json, "mapInfoSerialSize", mapInfoSerialSize);
        AddLong(json, "emittedRoles", emittedRoles);
        AddBool(json, "roleArraysConsumedExactly", true);
        AddBool(json, "runtimeSquadsExtracted", false);
        AddString(
            json,
            "runtimeSquadSource",
            "ROMapInfo.InitSquadsForGametype/GetNumSquads and live squad occupancy");
        EndRecord(output, json);
    }

    private static void EmitClass(TextWriter output, string className, int count)
    {
        StringBuilder json = BeginRecord("class");
        AddString(json, "class", className);
        AddLong(json, "exports", count);
        EndRecord(output, json);
    }

    private static void EmitClassSummary(
        TextWriter output, int matchedClasses, int emittedClasses, bool truncated)
    {
        StringBuilder json = BeginRecord("summary");
        AddString(json, "mode", "classes");
        AddLong(json, "matchedClasses", matchedClasses);
        AddLong(json, "emittedClasses", emittedClasses);
        AddBool(json, "truncated", truncated);
        EndRecord(output, json);
    }

    private static void EmitBrushBounds(
        TextWriter output,
        int ordinal,
        string className,
        string actorPath,
        int actorExportIndex,
        long actorSerialOffset,
        long actorSerialSize,
        string modelPath,
        int modelExportIndex,
        long modelSerialOffset,
        long modelSerialSize,
        int modelNetIndex,
        int noneNameIndex,
        VectorValue actorLocation,
        ModelBoundsValue bounds,
        VectorValue worldMin,
        VectorValue worldMax)
    {
        StringBuilder json = BeginRecord("brushBounds");
        AddLong(json, "schemaVersion", 1);
        AddLong(json, "ordinal", ordinal);
        AddString(json, "actorClass", className);
        AddString(json, "actorPath", actorPath);
        AddLong(json, "actorExportIndex", actorExportIndex);
        AddLong(json, "actorSerialOffset", actorSerialOffset);
        AddLong(json, "actorSerialSize", actorSerialSize);
        AddString(json, "modelPath", modelPath);
        AddLong(json, "modelExportIndex", modelExportIndex);
        AddLong(json, "modelSerialOffset", modelSerialOffset);
        AddLong(json, "modelSerialSize", modelSerialSize);
        AddLong(json, "modelNetIndex", modelNetIndex);
        AddLong(json, "taggedPropertyTerminatorNameIndex", noneNameIndex);
        AddString(json, "nativeLayout", "UObject.NetIndex+FName(None)+FBoxSphereBounds");
        AddString(json, "boundsSource", "UE3 UModel::Serialize FBoxSphereBounds");
        AddString(json, "geometryFidelity", "bounds_only");
        AddBool(json, "sourceBoundsVerified", true);
        AddBool(json, "collisionGeometryComplete", false);
        AddBool(json, "worldCollisionComplete", false);
        AddBool(json, "safeForGameplayOcclusion", false);
        AddRaw(json, "actorLocation", VectorJson(actorLocation));
        AddRaw(
            json,
            "transform",
            "{\"kind\":\"translation_only\",\"nonTranslationSource\":" +
            "\"UE3 Actor class defaults\",\"rotationUu\":{\"pitch\":0,\"yaw\":0,\"roll\":0}," +
            "\"drawScale\":1,\"drawScale3D\":{\"x\":1,\"y\":1,\"z\":1}," +
            "\"prePivot\":{\"x\":0,\"y\":0,\"z\":0}}");
        AddRaw(
            json,
            "localBounds",
            "{\"origin\":" + VectorJson(bounds.Origin) +
            ",\"extent\":" + VectorJson(bounds.Extent) +
            ",\"sphereRadius\":" + bounds.SphereRadius.ToString("R", Invariant) + "}");
        AddRaw(
            json,
            "worldAabb",
            "{\"min\":" + VectorJson(worldMin) +
            ",\"max\":" + VectorJson(worldMax) + "}");
        EndRecord(output, json);
    }

    private static void EmitBrushBoundsError(
        TextWriter output, string actorPath, string className, string message)
    {
        StringBuilder json = BeginRecord("error");
        AddString(json, "stage", "brush-bounds");
        AddString(json, "class", className);
        AddString(json, "path", actorPath);
        AddString(json, "message", Truncate(message, 2048));
        EndRecord(output, json);
    }

    private static void EmitBrushBoundsSummary(
        TextWriter output,
        int candidates,
        int emitted,
        bool truncated,
        int errors,
        bool stoppedOnErrors,
        int discoveredModels)
    {
        StringBuilder json = BeginRecord("summary");
        AddString(json, "mode", "brush-bounds");
        AddLong(json, "matchedActorCandidates", candidates);
        AddLong(json, "emittedBounds", emitted);
        AddBool(json, "truncated", truncated);
        AddLong(json, "errors", errors);
        AddBool(json, "stoppedOnErrorLimit", stoppedOnErrors);
        AddLong(json, "discoveredWorldModels", discoveredModels);
        AddString(json, "geometryFidelity", "bounds_only");
        AddBool(json, "collisionGeometryComplete", false);
        AddBool(json, "worldCollisionComplete", false);
        AddBool(json, "safeForGameplayOcclusion", false);
        EndRecord(output, json);
    }

    private static string VectorJson(VectorValue value)
    {
        return "{\"x\":" + value.X.ToString("R", Invariant) +
            ",\"y\":" + value.Y.ToString("R", Invariant) +
            ",\"z\":" + value.Z.ToString("R", Invariant) + "}";
    }

    private static void EmitActor(
        TextWriter output,
        int ordinal,
        string className,
        string classPath,
        string name,
        string path,
        int exportIndex,
        long serialOffset,
        long serialSize,
        int matchingProperties,
        bool propertiesTruncated,
        bool hasLocation,
        double locationX,
        double locationY,
        double locationZ,
        IList<ExtractedProperty> properties)
    {
        StringBuilder json = BeginRecord("actor");
        AddLong(json, "ordinal", ordinal);
        AddString(json, "class", className);
        AddString(json, "classPath", classPath);
        AddString(json, "name", name);
        AddString(json, "path", path);
        AddLong(json, "exportIndex", exportIndex);
        AddLong(json, "serialOffset", serialOffset);
        AddLong(json, "serialSize", serialSize);
        AddLong(json, "matchingPropertyCount", matchingProperties);
        AddBool(json, "propertiesTruncated", propertiesTruncated);
        if (hasLocation)
        {
            AddRaw(
                json,
                "location",
                "{\"x\":" + locationX.ToString("R", Invariant) +
                ",\"y\":" + locationY.ToString("R", Invariant) +
                ",\"z\":" + locationZ.ToString("R", Invariant) + "}");
        }

        json.Append(",\"properties\":[");
        for (int index = 0; index < properties.Count; index++)
        {
            if (index > 0)
            {
                json.Append(',');
            }
            ExtractedProperty property = properties[index];
            json.Append('{');
            json.Append("\"name\":"); AppendJsonString(json, property.Name);
            json.Append(",\"type\":"); AppendJsonString(json, property.Type);
            json.Append(",\"arrayIndex\":"); json.Append(property.ArrayIndex.ToString(Invariant));
            json.Append(",\"size\":"); json.Append(property.Size.ToString(Invariant));
            json.Append(",\"value\":"); AppendJsonString(json, property.Value);
            json.Append('}');
        }
        json.Append(']');
        EndRecord(output, json);
    }

    private static void EmitActorError(
        TextWriter output, string actorPath, string className, string message)
    {
        StringBuilder json = BeginRecord("error");
        AddString(json, "stage", "actor");
        AddString(json, "class", className);
        AddString(json, "path", actorPath);
        AddString(json, "message", Truncate(message, 2048));
        EndRecord(output, json);
    }

    private static void EmitActorSummary(
        TextWriter output,
        int candidates,
        int emitted,
        bool actorsTruncated,
        int propertiesTruncated,
        int errors,
        bool stoppedOnErrors)
    {
        StringBuilder json = BeginRecord("summary");
        AddString(json, "mode", "actors");
        AddLong(json, "matchedActorCandidates", candidates);
        AddLong(json, "emittedActors", emitted);
        AddBool(json, "actorsTruncated", actorsTruncated);
        AddLong(json, "actorsWithTruncatedProperties", propertiesTruncated);
        AddLong(json, "errors", errors);
        AddBool(json, "stoppedOnErrorLimit", stoppedOnErrors);
        EndRecord(output, json);
    }

    private static StringBuilder BeginRecord(string recordType)
    {
        StringBuilder json = new StringBuilder(1024);
        json.Append("{\"record\":");
        AppendJsonString(json, recordType);
        return json;
    }

    private static void EndRecord(TextWriter output, StringBuilder json)
    {
        json.Append('}');
        output.WriteLine(json.ToString());
    }

    private static void AddString(StringBuilder json, string name, string value)
    {
        json.Append(','); AppendJsonString(json, name); json.Append(':'); AppendJsonString(json, value);
    }

    private static void AddLong(StringBuilder json, string name, long value)
    {
        json.Append(','); AppendJsonString(json, name); json.Append(':'); json.Append(value.ToString(Invariant));
    }

    private static void AddBool(StringBuilder json, string name, bool value)
    {
        json.Append(','); AppendJsonString(json, name); json.Append(':'); json.Append(value ? "true" : "false");
    }

    private static void AddRaw(StringBuilder json, string name, string value)
    {
        json.Append(','); AppendJsonString(json, name); json.Append(':'); json.Append(value);
    }

    private static void AppendJsonString(StringBuilder output, string value)
    {
        if (value == null)
        {
            output.Append("null");
            return;
        }

        output.Append('"');
        foreach (char character in value)
        {
            switch (character)
            {
                case '"': output.Append("\\\""); break;
                case '\\': output.Append("\\\\"); break;
                case '\b': output.Append("\\b"); break;
                case '\f': output.Append("\\f"); break;
                case '\n': output.Append("\\n"); break;
                case '\r': output.Append("\\r"); break;
                case '\t': output.Append("\\t"); break;
                default:
                    if (character < 0x20)
                    {
                        output.Append("\\u");
                        output.Append(((int)character).ToString("x4", Invariant));
                    }
                    else
                    {
                        output.Append(character);
                    }
                    break;
            }
        }
        output.Append('"');
    }

    private static T Quiet<T>(Func<T> action)
    {
        TextWriter originalOutput = Console.Out;
        TextWriter originalError = Console.Error;
        try
        {
            Console.SetOut(TextWriter.Null);
            Console.SetError(TextWriter.Null);
            return action();
        }
        finally
        {
            Console.SetOut(originalOutput);
            Console.SetError(originalError);
        }
    }

    private static void Quiet(Action action)
    {
        Quiet(delegate
        {
            action();
            return true;
        });
    }

    private static int CollectionCount(object collection, string label)
    {
        ICollection nongeneric = collection as ICollection;
        if (nongeneric == null)
        {
            throw new InvalidDataException(label + " is not a collection");
        }
        return nongeneric.Count;
    }

    private static bool HasHelp(string[] args)
    {
        if (args == null)
        {
            return false;
        }
        foreach (string argument in args)
        {
            if (argument == "--help" || argument == "-h" || argument == "/?")
            {
                return true;
            }
        }
        return false;
    }

    private static string NextValue(string[] args, ref int index, string argument)
    {
        if (++index >= args.Length)
        {
            throw new UsageException(argument + " requires a value");
        }
        return args[index];
    }

    private static int ParseInt(string value, string argument, int minimum, int maximum)
    {
        int parsed;
        if (!int.TryParse(value, NumberStyles.None, Invariant, out parsed) ||
            parsed < minimum || parsed > maximum)
        {
            throw new UsageException(
                argument + " must be an integer from " + minimum.ToString(Invariant) +
                " through " + maximum.ToString(Invariant));
        }
        return parsed;
    }

    private static long ParseLong(string value, string argument, long minimum, long maximum)
    {
        long parsed;
        if (!long.TryParse(value, NumberStyles.None, Invariant, out parsed) ||
            parsed < minimum || parsed > maximum)
        {
            throw new UsageException(
                argument + " must be an integer from " + minimum.ToString(Invariant) +
                " through " + maximum.ToString(Invariant));
        }
        return parsed;
    }

    private static string FullPath(string path, string label)
    {
        try
        {
            return Path.GetFullPath(path);
        }
        catch (Exception ex)
        {
            throw new ValidationException("invalid " + label + " path: " + ex.Message);
        }
    }

    private static bool SamePath(string left, string right)
    {
        return string.Equals(
            left.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
            right.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
            StringComparison.OrdinalIgnoreCase);
    }

    private static string SafeString(object value)
    {
        return value == null ? string.Empty : Convert.ToString(value, Invariant) ?? string.Empty;
    }

    private static Exception Unwrap(Exception exception)
    {
        Exception current = exception;
        while (current is TargetInvocationException && current.InnerException != null)
        {
            current = current.InnerException;
        }
        return current;
    }

    private static string ExceptionMessage(Exception exception)
    {
        if (exception == null)
        {
            return "unknown error";
        }
        string message = exception.GetType().Name + ": " + exception.Message;
        return Truncate(message, 2048);
    }

    private static string Truncate(string value, int maximum)
    {
        if (value == null || value.Length <= maximum)
        {
            return value;
        }
        return value.Substring(0, maximum);
    }

    private static void WriteError(string stage, string message)
    {
        Console.Error.WriteLine("ERROR [" + stage + "]: " + message);
    }

    private static void PrintUsage(TextWriter output)
    {
        output.WriteLine("CookedMapMetadataExtractor - bounded, read-only RS2 package metadata extraction");
        output.WriteLine();
        output.WriteLine("Required:");
        output.WriteLine("  --input PATH              Cooked .roe map or ROGame.u package to inspect");
        output.WriteLine("  --uelib PATH              Eliot.UELib.dll (tested with 1.12.1)");
        output.WriteLine();
        output.WriteLine("Selection/output:");
        output.WriteLine("  --mode actors|classes|role-info|role-exports|brush-bounds");
        output.WriteLine("                           Actor records (default), class inventory,");
        output.WriteLine("                           exact ROMapInfo arrays, paired ROGame role exports, or");
        output.WriteLine("                           source-verified UModel bounds diagnostics");
        output.WriteLine("  --output PATH             JSONL file; stdout when omitted");
        output.WriteLine("  --overwrite               Replace an existing output file");
        output.WriteLine("  --class-pattern REGEX     Full class-name match");
        output.WriteLine("  --property-pattern REGEX  Full property-name match");
        output.WriteLine();
        output.WriteLine("Bounds:");
        output.WriteLine("  --max-input-bytes N       Default 536870912 (512 MiB)");
        output.WriteLine("  --max-exports N           Default 250000");
        output.WriteLine("  --max-actors N            Default 1000");
        output.WriteLine("  --max-properties N        Default 64 per actor");
        output.WriteLine("  --max-value-chars N       Default 1024 per property value");
        output.WriteLine("  --max-errors N            Default 25");
        output.WriteLine("  --max-classes N           Default 512 in classes mode");
        output.WriteLine("  --self-test-role-schema   Run synthetic exact-role-schema checks");
        output.WriteLine("  --self-test-role-export-schema");
        output.WriteLine("                           Run role-name/static-reference checks");
        output.WriteLine();
        output.WriteLine("Role-export artifact pinning:");
        output.WriteLine("  --expected-package-guid H Exact nonzero 32-digit ROGame package GUID");
        output.WriteLine("  --expected-sha256 H       Exact nonzero 64-digit ROGame.u SHA-256");
        output.WriteLine("  --object-base N           Optional PackageMap base for derived references");
    }
}
