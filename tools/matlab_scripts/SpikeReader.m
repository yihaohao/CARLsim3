classdef SpikeReader < handle
    % SR = SpikeReader(spikeFile) creates a new instance of class SpikeReader.
    %
    % SpikeReader can be used to read spike files generated by the SpikeMonitor
    % utility in CARLsim.
    %
    %
    % Version 10/2/2014
    % Author: Michael Beyeler <mbeyeler@uci.edu>
    
    %% PROPERTIES
    properties (SetAccess = private)
        fileStr;            % path to spike file
        fileId;             % file ID of spike file
        
        fileSignature;      % int signature of all spike files
        fileVersion;        % required version number
        fileSizeByteHeader; % byte size of header section
    end
    
    
    %% PUBLIC METHODS
    methods
        function obj = SpikeReader(spikeFile)
            % SR = SpikeReader(spikeFile) creates a new instance of class
            % SpikeReader, which can be used to read spike files generated by
            % the SpikeMonitor utility in CARLsim.
            %
            % SPIKEFILE   - Path to spike file (expects data to be in Adress-
            %               Event-Representation AER: spike time (ms) followed
            %               by neuron ID (both int32)), like the ones created
            %               by the CARLsim SpikeMonitor utility.
            if nargin<1,error('Path to spike file needed'),end
            
            obj.fileStr = spikeFile;
            obj.privLoadDefaultParams();
            
            % move unsafe code out of constructor
            obj.privOpenFile()
        end
        
        function delete(obj)
            % destructor, implicitly called to fclose file
            if obj.fileId ~= -1
                fclose(obj.fileId);
            end
        end
        
        function spk = readSpikes(obj, frameDur)
            % spk = readSpikes(frameDur) reads the spike file and arranges
            % spike times into bins of frameDur millisecond length.
            %
            % Returns a 2-D matrix (spike times x neuron IDs), 1-indexed.
            %
            % FRAMEDUR    - Size of binning window for spike times (ms).
            %               Set frameDur to -1 in order to get the spikes in
            %               AER format [times;nIDs].
            %               Default: 1000.
            if nargin<1,frameDur=1000;end
            
            % rewind file pointer, skip header
            fseek(obj.fileId, obj.fileSizeByteHeader, 'bof');
            nrRead=1e6;
            d=zeros(0,nrRead);
            spk=[];
            
            while size(d,2)==nrRead
                % D is a 2xNRREAD matrix.  Row 1 contains the times that
                % the neuron spiked. Row 2 contains the neuron id that
                % spiked at this corresponding time.
                d = fread(obj.fileId, [2 nrRead], 'int32');
                
                if ~isempty(d)
                    if frameDur<0
                        % Return data in AER format, i.e.: [time;nID]
                        % Note: Using SPARSE on large matrices that mostly
                        % contain 0 is inefficient (-> "big sparse matrix")
                        spk = [spk, d];
                    else
                        % Resulting matrix s will have rows corresponding
                        % to time values with a minimum value of 1 and
                        % columns organized by neuron ids that are indexed
                        % starting with 1.  FRAMEDUR effectively bins the
                        % data. FRAMEDUR=1 bins at 1 ms, FRAMEDUR=1000 bins
                        % at 1000 ms, etc.
                        
                        % Initialize the entire S matrix to 0 with the
                        % correct dimensions.
                        maxR = floor(d(1,end)/frameDur)+1;
                        maxC = max(d(2,:))+1;
                        if size(spk,1)~=maxR || size(spk,2)~=maxC
                            spk(maxR, maxC)=0;
                        end
                        
                        % Use sparse matrix to create a matrix S with
                        % correct dimensions. All firing events for each
                        % neuron id and time bin are summed automatically
                        % with ACCUMARRAY.  Finally the matrix is resized
                        % to include all the zero entries with the correct
                        % matrix dimensions. ACCUMARRAY is supposed to be
                        % faster than full(sparse(...)). Make sure the
                        % first two arguments are column vectors.
                        subs = [floor(d(1,:)/frameDur)'+1,d(2,:)'+1];
                        spk = spk + accumarray(subs, 1, size(spk));
                    end
                end
            end
            
        end
        
    end
    
    %% PRIVATE METHODS
    methods (Hidden, Access = private)
        function privLoadDefaultParams(obj)
            obj.fileId = -1;
            obj.fileSignature = 206661989;
            obj.fileVersion = 1.0;
            obj.fileSizeByteHeader = 8;
        end
        
        function privOpenFile(obj)
            % try to open spike file
            obj.fileId = fopen(obj.fileStr,'r');
            if obj.fileId==-1
                error(['Could not open file "' obj.fileStr ...
                    '" with read permission'])
            end
            
            % read signature
            sign = fread(obj.fileId, 1, 'int32');
            if sign~=obj.fileSignature
                error('Unknown file type')
            end
            
            % read version number
            version = fread(obj.fileId, 1, 'float32');
            if (version ~= obj.fileVersion)
                error(['Unknown file version, must have Version 1.0 (Version ' ...
                    num2str(version) ' found)'])
            end
            
            
        end
    end
end