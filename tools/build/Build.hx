class Build extends hxcpp.Builder
{
   // Create a build in 'bin' directory, with the "stdlibc++" flags for compatibility
   //  This flasg should not make a difference because hxcpp does not use stdlibc++
   override public function wantLegacyIosBuild() { return true; }

   override public function wantWindows64() { return true; }


   // Override to ensure this version if hxcpp is used, even if haxelib says otherwise
   override public function runBuild(target:String, isStatic:Bool, arch:String, inFlags:Array<String>)
   {
       var args = ["run.n", "Build.xml"].concat(inFlags);
       var here = Sys.getCwd().split("\\").join("/");

       var parts = here.split("/");
       if (parts.length>0 && parts[parts.length-1]=="")
          parts.pop();
       if (parts.length>0)
          parts.pop();
       var hxcppDir = parts.join("/");

#if tivo
       //build.n must be executed from the "project" directory.
       //By default hxcpp will set the CWD to the parent folder
       //(which is the root of hxcpp) before calling "neko run.n" to simulate
       //a call from haxelib.
       //
       //Since in our case HAXELIB_STAGED_DIR only contains symlinks to hxcpp
       //in SRCROOT and we need to "cd" into the symlinked "project" folder to run build.n,
       //we can't set the CWD to the parent since it would point
       //to the hxcpp folder in SRCROOT.
       //This would cause neko to fail to find "run.n" since we
       //built it in HAXELIB_STAGED_DIR.
       //
       //Instead we set the root of hxcpp to the staged hxcpp in OBJROOT
       if (Sys.getEnv("HAXELIB_STAGED_DIR") != null) {
           hxcppDir = Sys.getEnv("HAXELIB_STAGED_DIR");
       }
#end

       // This is how haxelib calls a 'run.n' script...
       Sys.setCwd(hxcppDir);
       args.push(here);
       Sys.println("neko " + args.join(" ")); 
       if (Sys.command("neko",args)!=0)
       {
          Sys.println("#### Error building neko " + inFlags.join(" "));
          Sys.exit(-1);
       }
       Sys.setCwd(here);
   }

   public static function main()
   {
      new Build( Sys.args() );
   }
}
