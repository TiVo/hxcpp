import Dependencies;

class FileAndDependencies {
   public var mFile:File;
   public var mDependencies:Dependencies;

   public function new(file:File, dependencies:Dependencies) {
      mFile = file;
      mDependencies = dependencies;
   }
}
