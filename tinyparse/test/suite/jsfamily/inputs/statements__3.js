import defaultMember from "module-name";
import * as name from "module-name";
import { member } from "module-name";
import { member1 , member2 } from "module-name";
import { member1 , member2 as alias2 } from "module-name";
import { "string name" as alias } from "module-name";
import defaultMember, { member1, member2 as alias2 } from "module-name";
import defaultMember, * as name from "module-name";
import "module-name";
import { member1 , member2 as alias2, } from "module-name";
import pkg from "./package.json" with { type: "json" };
import("a");
import("a").then((m) => {});
import.meta.url;
import { b } from
'b'; /* comment */ import
 { a } from 'a';
