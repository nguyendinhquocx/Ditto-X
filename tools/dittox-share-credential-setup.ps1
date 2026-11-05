# dittox-share-credential-setup.ps1 — cấu hình Ditto credential cho tính năng Ctrl+Z
# (SPEC-2026-11-05-dittox-quick-handoff-public-clip).
#
# Nhập secret qua secure prompt (không echo), ghi vào Windows Credential Manager
# dưới target "Ditto-X Share API Credential". Giá trị KHÔNG được in ra màn hình,
# log hay file. Ditto (DittoXCredentialStore) đọc đúng target này.
#
# Dùng:
#   powershell -ExecutionPolicy Bypass -File .\dittox-share-credential-setup.ps1            # đặt/xoá rồi đặt mới
#   powershell -ExecutionPolicy Bypass -File .\dittox-share-credential-setup.ps1 -Remove    # xoá entry
#
# Key được cấp kèm Worker secret (xem docs/operations.md của share.quoc.app);
# không bao giờ paste key vào chat/spec/source code.

param(
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$TargetName = 'Ditto-X Share API Credential'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class DittoXCred
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct CREDENTIAL
    {
        public int Flags;
        public int Type;
        public string TargetName;
        public string Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public int CredentialBlobSize;
        public IntPtr CredentialBlob;
        public int Persist;
        public int AttributeCount;
        public IntPtr Attributes;
        public string TargetAlias;
        public string UserName;
    }

    [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CredWrite(ref CREDENTIAL credential, int flags);

    [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CredDelete(string targetName, int type, int flags);

    public const int CRED_TYPE_GENERIC = 1;
    public const int CRED_PERSIST_LOCAL_MACHINE = 2;
}
"@

function Remove-ExistingCredential
{
    try
    {
        [void][DittoXCred]::CredDelete($TargetName, [DittoXCred]::CRED_TYPE_GENERIC, 0)
        Write-Host "Đã xoá entry cũ (nếu có)."
    }
    catch
    {
        # Không có entry cũ — bỏ qua.
    }
}

if ($Remove)
{
    Remove-ExistingCredential
    Write-Host "Đã xoá Ditto credential. Ctrl+Z sẽ báo 'chưa cấu hình credential'."
    exit 0
}

# Secure prompt — ký tự không echo ra màn hình.
$secureKey = Read-Host -Prompt 'Nhập Ditto credential (input ẩn, Enter để xác nhận)' -AsSecureString

if ($secureKey.Length -eq 0)
{
    Write-Error 'Key rỗng — không ghi gì cả.'
    exit 1
}

# Giải SecureString trong bộ nhớ, ghi ngay vào Credential Manager rồi zero hoá.
$bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureKey)
$credentialWritten = $false
try
{
    $plainLength = [System.Runtime.InteropServices.Marshal]::StringLen($bstr)
    $bytes = New-Object byte[] $plainLength
    for ($i = 0; $i -lt $plainLength; $i++)
    {
        $bytes[$i] = [byte][System.Runtime.InteropServices.Marshal]::ReadByte($bstr, $i)
    }

    $ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($plainLength)
    try
    {
        [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $ptr, $plainLength)

        $credential = New-Object DittoXCred+CREDENTIAL
        $credential.Flags = 0
        $credential.Type = [DittoXCred]::CRED_TYPE_GENERIC
        $credential.TargetName = $TargetName
        $credential.Comment = 'Ditto-X Ctrl+Z public clip (SPEC-2026-11-05). Rotate bằng script này.'
        $credential.CredentialBlobSize = $plainLength
        $credential.CredentialBlob = $ptr
        $credential.Persist = [DittoXCred]::CRED_PERSIST_LOCAL_MACHINE
        $credential.AttributeCount = 0
        $credential.Attributes = [System.IntPtr]::Zero
        $credential.TargetAlias = [string]::Empty
        $credential.UserName = [string]::Empty

        if (-not [DittoXCred]::CredWrite([ref]$credential, 0))
        {
            throw "CredWrite lỗi (Win32 code $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
        }
        $credentialWritten = $true
    }
    finally
    {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
    }
}
finally
{
    # Zero hoá buffer trước khi trả bộ nhớ — key không còn sót trong process sau script.
    if ($bstr -ne [System.IntPtr]::Zero)
    {
        [System.Runtime.InteropServices.Marshal]::ZeroBSTR($bstr) 2>$null
        [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
    if ($bytes) { [System.Array]::Clear($bytes, 0, $bytes.Length) }
}

if ($credentialWritten)
{
    Write-Host ""
    Write-Host "OK: đã lưu Ditto credential vào Windows Credential Manager."
    Write-Host "Target: $TargetName"
    Write-Host "Kiểm tra: nhấn Ctrl+Z trên một clip trong Ditto list để publish."
}
else
{
    Write-Error 'Không ghi được credential.'
    exit 1
}
