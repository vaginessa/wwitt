<?php
/**
 * This is just for faster exploit development.
 */

//Dont save the results
define("CLI_DEBUG", true);

include("index.php");

if(!$argv[1] || !$argv[2]){
	die(echoCli("Example: php testCli.php httpWeakLogin 192.168.1.1", "failure"));
}

$execTime = microtime(true);

//Define
$vuln = new Vuln($argv[1]);

//Creating Models
$host = new Host();
$host->ipAdress = $argv[2];
if(!$host->ipAdress){
	die(echoCli("Host not found", "failure"));
}
$virtualhost = null;

//Launch vuln exploit
if(!$vuln->module){
	die(echoCli("Vuln not found", "failure"));
}
$res = $vuln->exploit($host, $vuln->port, $virtualhost);

//Result
echoCli("Result:", "title");
if($res){
	echoCli("Vuln exploited!", "success");
}else{
	echoCli("Exploit failed", "failure");
}
//Show messages
print_r(Registry::getMessages());
echo "\n";

echoCli("Time: ".(microtime(true)-$execTime)."ms", "notice");

?>