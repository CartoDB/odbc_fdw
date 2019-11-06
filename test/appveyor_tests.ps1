# Taken from https://deadroot.info/scripts/2018/09/04/PowerShell-Templating
function Merge-Tokens($template, $tokens)
{
  return [regex]::Replace(
    $template,
    '\$\{(?<tokenName>\w+)\}',
    {
      param($match)
      $tokenName = $match.Groups['tokenName'].Value
      return $tokens[$tokenName]
    })
}

# https://www.appveyor.com/docs/services-databases
$Config = @{
  mysql = @{
    driver = 'MySQL ODBC 5.3 Unicode Driver'
    host = 'localhost'
    port = '3306'
    dbname = 'fdw_tests'
    encoding = 'utf8'
    user = 'root'
    password = 'Password12!'
  }
  postgres = @{
    driver = 'PostgreSQL Unicode(x64)'
    host = 'localhost'
    port = '5432'
    dbname = 'fdw_tests'
    user = 'postgres'
    password = 'Password12!'
  }
  sqlserver = @{
    driver = 'ODBC Driver 13 for SQL Server'
    host = '(local)\SQL2017'
    port = '1433'
    dbname = 'master'
    user = 'sa'
    password = 'Password12!'
  }
}

foreach ($c in $Config.GetEnumerator()) {
  $tpl = Get-Content "$PSScriptRoot\template\$($c.Name)_installation_test.tpl" -Raw
  $generated_test = Merge-Tokens $tpl $($c.Value)
  Set-Content -Path "$PSScriptRoot\sql/$($c.Name)_10_installation_test.sql" -Value $generated_test
  Set-Content -Path "$PSScriptRoot\expected\$($c.Name)_10_installation_test.out" -Value $generated_test
}

$env:Path += ";C:\Program Files\MySQL\MySQL Server 5.7\bin"
& mysql -e "create database fdw_tests character set utf8mb4 collate utf8mb4_unicode_ci;" --user=root
& cmd.exe /c 'mysql fdw_tests --user=root < test\fixtures\mysql_fixtures.sql'
& createdb fdw_tests
& psql -f test\fixtures\postgres_fixtures.sql fdw_tests postgres 2>&1 |
  %{ if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { $_ } } |
  Out-Default
& sqlcmd -S "(local)\SQL2017" -U "sa" -d master -i "$PSScriptRoot\fixtures\sqlserver_fixtures.sql"
Rename-Item -Path "$PSScriptRoot\sql\sqlserver_20_query_test_disabled.sql" -NewName "sqlserver_20_query_test.sql"

if (-not (Test-Path "$PSScriptRoot\..\psqlodbc_x64.msi")) {
  Start-FileDownload "https://ftp.postgresql.org/pub/odbc/versions/msi/psqlodbc_12_00_0000-x64.zip"
  Expand-Archive -LiteralPath psqlodbc_12_00_0000-x64.zip -DestinationPath .
  Remove-Item psqlodbc_12_00_0000-x64.zip
}
& msiexec /i psqlodbc_x64.msi /qn /quiet

Add-AppveyorTest Regression -Framework pg_regress -FileName sql\ -Outcome Running
$env:Outcome="Passed"
$elapsed=(Measure-Command {
  pg_regress "--bindir=$env:pgroot\bin" --inputdir=test --outputdir=test --load-extension=odbc_fdw --dbname=regression `
    mysql_10_installation_test mysql_20_query_test `
    postgres_10_installation_test postgres_20_query_test `
    sqlserver_10_installation_test sqlserver_20_query_test `
    2>&1 |
    %{ if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { $_ } } |
      Out-Default
  if ($LASTEXITCODE -ne 0) {
    $env:Outcome="Failed"
  }
}).TotalMilliseconds
Update-AppVeyorTest Regression -Framework pg_regress -FileName sql\ -Outcome "$env:Outcome" -Duration $elapsed
if ("$env:Outcome" -ne "Passed") {
  type test\regression.diffs
  $host.SetShouldExit($LastExitCode)
}
