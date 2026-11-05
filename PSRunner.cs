//==============================================================================
// PSRunner.cs - managed payload, executed by psrun_host.dll via
//               ICLRRuntimeHost::ExecuteInDefaultAppDomain
//------------------------------------------------------------------------------
// The native host calls PSRunner.Entry.Run(string) with the raw argument
// string. Grammar (put -out / -raw BEFORE -c):
//
//   -c <script>             run script text (rest of the line)
//   -f <path.ps1> [args]    run script file; rest is passed to the script
//   -out <file>             output file   (default %TEMP%\psrun.log)
//   -raw                    no Out-String formatting (raw objects)
//
// C# 5 compatible: build with the framework csc.exe, see build.cmd.
//==============================================================================

using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Management.Automation;
using System.Management.Automation.Runspaces;
using System.Text;
using System.Threading;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace PSRunner
{
    public static class Entry
    {
        private const string Usage =
            "psrun: onbekende args.\r\n" +
            "  -con                   console-window shell op de target (typen + output)\r\n" +
            "  -shell <dir>           file-based shell: *.in -> *.out\r\n" +
            "  -c <script>            one-shot script (rest van de regel)\r\n" +
            "  -f <path.ps1> [args]   one-shot script file\r\n" +
            "  ('exit' sluit de console shell)";

        // static int(string) - signature required by ExecuteInDefaultAppDomain
        public static int Run(string args)
        {
            string outPath = Path.Combine(Path.GetTempPath(), "psrun.log");
            bool raw = false;
            string shellDir = null;
            bool conMode = false;
            string script = null;

            StringBuilder log = new StringBuilder();
            log.AppendLine("=== psrun " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") +
                           " | host: " + Process.GetCurrentProcess().ProcessName +
                           " pid " + Process.GetCurrentProcess().Id + " ===");

            string rest = args ?? "";
            while (true)
            {
                string tok, next;
                NextToken(rest, out tok, out next);
                if (tok == null) break;
                rest = next;

                if (tok == "-out")
                {
                    string v, r;
                    NextToken(rest, out v, out r);
                    rest = r;
                    if (!String.IsNullOrEmpty(v)) outPath = v;
                }
                else if (tok == "-raw")
                {
                    raw = true;
                }
                else if (tok == "-con")
                {
                    conMode = true;
                }
                else if (tok == "-shell")
                {
                    string d, r2;
                    NextToken(rest, out d, out r2);
                    rest = r2;
                    shellDir = String.IsNullOrEmpty(d)
                        ? Path.Combine(Path.GetTempPath(), "psrun_shell")
                        : d;
                }
                else if (tok == "-c")
                {
                    script = rest;
                    break;
                }
                else if (tok == "-f")
                {
                    string path, r;
                    NextToken(rest, out path, out r);
                    rest = r;
                    // & 'script' <rest> : rest becomes the script arguments
                    script = "& '" + path + "'";
                    if (rest.Trim().Length > 0) script += " " + rest;
                    break;
                }
                else
                {
                    log.AppendLine("unknown arg: " + tok);
                }
            }

            if (shellDir != null) return ShellLoop(shellDir);
            if (conMode) return ConsoleShell();

            int rc = 0;
            if (script == null || script.Trim().Length == 0)
            {
                log.Append(Usage);
                rc = 3;
            }
            else
            {
                Runspace runspace = null;
                PowerShell ps = null;
                try
                {
                    runspace = RunspaceFactory.CreateRunspace();
                    runspace.Open();

                    ps = PowerShell.Create();
                    ps.Runspace = runspace;

                    // note: trailing #-comments break the appended pipe; use -raw
                    ps.AddScript(raw ? script : script + " | Out-String -Width 200");

                    Collection<PSObject> output = ps.Invoke();
                    if (output != null)
                    {
                        foreach (object o in output)
                            log.AppendLine(o == null ? "<null>" : o.ToString());
                    }
                    if (ps.HadErrors && ps.Streams.Error != null)
                    {
                        foreach (ErrorRecord e in ps.Streams.Error)
                            log.AppendLine("[E] " + e);
                        rc = 1;
                    }
                }
                catch (Exception ex)
                {
                    log.AppendLine("[X] " + ex);
                    rc = 2;
                }
                finally
                {
                    if (ps != null) ps.Dispose();
                    if (runspace != null)
                    {
                        try { runspace.Close(); }
                        catch { /* ignore */ }
                        runspace.Dispose();
                    }
                }
            }

            File.WriteAllText(outPath, log.ToString(), new UTF8Encoding(false));
            return rc;
        }

        // ------------------------------------------------------------------
        // console shell: opent een echt console-window op de desktop van de
        // session waarin regsvr32 draait. typen = PS, zelfde runspace.
        // (AllocConsole levert het venster; conhost is Windows-onderdeel)
        // ------------------------------------------------------------------
        [DllImport("kernel32.dll")] private static extern bool AllocConsole();
        [DllImport("kernel32.dll")] private static extern IntPtr GetConsoleWindow();
        [DllImport("kernel32.dll")] private static extern IntPtr CreateFileW(
            string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
        [DllImport("kernel32.dll")] private static extern bool SetStdHandle(int nStdHandle, IntPtr h);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool SetConsoleTitleW(string t);

        // bind .NET Console expliciet aan het nieuwe console-window; een launcher
        // (C2) redirect anders de std-handles en ReadLine leest een dode pipe.
        private static void BindConHandles()
        {
            const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
            const uint FILE_SHARE_READ = 0x1, FILE_SHARE_WRITE = 0x2, OPEN_EXISTING = 3;
            IntPtr hIn = CreateFileW("CONIN$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
            IntPtr hOut = CreateFileW("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);

            if (hIn != IntPtr.Zero && hIn != (IntPtr)(-1))
            {
                SetStdHandle(-10, hIn);   // STD_INPUT_HANDLE
                Console.SetIn(new StreamReader(new FileStream(
                    new SafeFileHandle(hIn, false), FileAccess.Read)));
            }
            if (hOut != IntPtr.Zero && hOut != (IntPtr)(-1))
            {
                SetStdHandle(-11, hOut);  // STD_OUTPUT_HANDLE
                StreamWriter w = new StreamWriter(new FileStream(
                    new SafeFileHandle(hOut, false), FileAccess.Write));
                w.AutoFlush = true;
                Console.SetOut(w);
            }
        }

        private static void FatalLog(Exception ex)
        {
            try
            {
                File.WriteAllText(Path.Combine(Path.GetTempPath(), "psrun_fatal.log"),
                    DateTime.Now + "\r\n" + ex);
            }
            catch { /* ignore */ }
        }

        private static int ConsoleShell()
        {
            try
            {
                if (GetConsoleWindow() == IntPtr.Zero) AllocConsole();
                BindConHandles();
                SetConsoleTitleW("psrun - in-proc PowerShell (regsvr32)");
            }
            catch (Exception ex) { FatalLog(ex); return 2; }

            Runspace rs = RunspaceFactory.CreateRunspace();
            rs.Open();
            try
            {
                Console.WriteLine("=== psrun console shell | pid " +
                    Process.GetCurrentProcess().Id + " | 'exit' = sluiten ===");
                while (true)
                {
                    Console.Write("PS> ");
                    string line = Console.ReadLine();
                    if (line == null) break;
                    if (line.Trim().Length == 0) continue;
                    if (line.Trim().Equals("exit", StringComparison.OrdinalIgnoreCase)) break;

                    PowerShell ps = PowerShell.Create();
                    ps.Runspace = rs;
                    ps.AddScript(line + " | Out-String -Width 200");
                    try
                    {
                        Collection<PSObject> output = ps.Invoke();
                        if (output != null)
                        {
                            foreach (object o in output)
                                Console.WriteLine(o == null ? "" : o.ToString());
                        }
                        if (ps.HadErrors && ps.Streams.Error != null)
                        {
                            foreach (ErrorRecord e in ps.Streams.Error)
                                Console.WriteLine("[E] " + e);
                        }
                    }
                    catch (Exception ex) { Console.WriteLine("[X] " + ex.Message); }
                    finally { ps.Dispose(); }
                }
            }
            finally
            {
                try { rs.Close(); } catch { /* ignore */ }
                rs.Dispose();
            }
            return 0;
        }

        // ------------------------------------------------------------------
        // persistent shell (file based): draait in de regsvr32 host zolang de loop loopt.
        // protocol: <dir>\NAME.in  (script, multi-line PS)  ->  NAME.out
        //           state (variabelen/cd/functies) blijft in dezelfde runspace;
        //           letterlijk 'exit' als complete inhoud beeindigt de shell.
        // ------------------------------------------------------------------
        private static int ShellLoop(string dir)
        {
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);

            string logPath = Path.Combine(dir, "shell.log");
            File.WriteAllText(logPath,
                "=== psrun shell " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") +
                " | pid " + Process.GetCurrentProcess().Id + " ===\r\n" +
                "drop NAME.in, read NAME.out - 'exit' stops the shell\r\n");

            Runspace rs = RunspaceFactory.CreateRunspace();
            rs.Open();
            try
            {
                bool quit = false;
                while (!quit)
                {
                    string[] pending = Directory.GetFiles(dir, "*.in");
                    Array.Sort(pending, StringComparer.OrdinalIgnoreCase);

                    foreach (string f in pending)
                    {
                        string name = Path.GetFileNameWithoutExtension(f);
                        string outPath = Path.Combine(dir, name + ".out");
                        StringBuilder sb = new StringBuilder();
                        sb.AppendLine("--- " + DateTime.Now.ToString("HH:mm:ss") + " ---");

                        string script = null;
                        try { script = File.ReadAllText(f); }
                        catch (Exception ex) { sb.AppendLine("[X] read: " + ex); }

                        bool wasExit = script != null &&
                                       script.Trim().Equals("exit", StringComparison.OrdinalIgnoreCase);

                        if (script != null && script.Trim().Length > 0 && !wasExit)
                        {
                            PowerShell ps = PowerShell.Create();
                            ps.Runspace = rs;
                            ps.AddScript(script + " | Out-String -Width 200");
                            try
                            {
                                Collection<PSObject> output = ps.Invoke();
                                if (output != null)
                                {
                                    foreach (object o in output)
                                        sb.AppendLine(o == null ? "" : o.ToString());
                                }
                                if (ps.HadErrors && ps.Streams.Error != null)
                                {
                                    foreach (ErrorRecord e in ps.Streams.Error)
                                        sb.AppendLine("[E] " + e);
                                }
                            }
                            catch (Exception ex) { sb.AppendLine("[X] " + ex); }
                            finally { ps.Dispose(); }
                        }

                        if (wasExit) sb.AppendLine("bye");

                        File.WriteAllText(outPath, sb.ToString(), new UTF8Encoding(false));
                        File.Delete(f);

                        if (wasExit) { quit = true; break; }
                    }

                    if (!quit) Thread.Sleep(300);
                }
            }
            finally
            {
                try { rs.Close(); } catch { /* ignore */ }
                rs.Dispose();
                File.AppendAllText(logPath, "shell exit " +
                    DateTime.Now.ToString("HH:mm:ss") + "\r\n", new UTF8Encoding(false));
            }
            return 0;
        }

        // split one token, honouring "quoted strings"
        private static void NextToken(string s, out string token, out string rest)
        {
            token = null;
            rest = "";
            if (s == null) return;

            s = s.TrimStart();
            if (s.Length == 0) return;

            if (s[0] == '"')
            {
                int end = s.IndexOf('"', 1);
                if (end < 0) { token = s.Substring(1); rest = ""; }
                else { token = s.Substring(1, end - 1); rest = s.Substring(end + 1); }
                return;
            }

            int sp = s.IndexOf(' ');
            if (sp < 0) { token = s; rest = ""; }
            else { token = s.Substring(0, sp); rest = s.Substring(sp + 1); }
        }
    }
}
