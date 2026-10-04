`one line`;
`multi
  line`;

`multi
  ${2 + 2}
  hello
  ${1 + 1, 2 + 2}
  line`;

`$$$$`;
`$$$$${ 1 }`;

`(a|b)$`;

`$`;

`$${'$'}$$${'$'}$$$$`;

`\ `;

`The command \`git ${args.join(' ')}\` exited with an unexpected code: ${exitCode}. The caller should either handle this error, or expect that exit code.`;

`\\`;

`//`;
